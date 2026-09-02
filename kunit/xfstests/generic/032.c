// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/032 over a loopback NFS mount: writeback racing a
 * concurrent sync.
 *
 * Upstream reproduces an XFS data-corruption bug: with sub-page filesystem
 * blocks, inode lock contention during writeback of pages over unwritten
 * extents made the extents fail to convert on I/O completion, and unwritten
 * extents read back as zeroes. It seeds a delayed-allocation block in each
 * page of the first 64K, preallocates that range, overwrites 1 MB over the
 * top, fsyncs, and asserts via fiemap that no unwritten extent survives
 * before EOF -- all while a background loop calls syncfs continuously.
 *
 * Two thirds of that has no NFS meaning. There are no unwritten extents (the
 * concept is an on-disk extent state, and the client sees a file), and fiemap
 * is not part of NFSv4.2, so the fiemap assertion cannot be ported at all.
 * What survives is the part that stresses the client rather than the disk
 * format: a scattered set of sub-page writes, an ALLOCATE over the same
 * range, a 1 MB overwrite and an fsync, with a sync running concurrently
 * throughout, and the requirement that every byte reads back correctly. The
 * ALLOCATE is real -- mode 0 is one of the two modes nfs42_fallocate()
 * accepts (fs/nfs/nfs4file.c:228) -- so this issues genuine ALLOCATE and
 * WRITE RPCs while another thread drives the client's writeback path.
 *
 * That concurrency is the reason to port it. Every other case in this set is
 * single-threaded; this is the only one where a second thread is inside the
 * NFS client at the same time as the writer, which is what makes it a check
 * on locking rather than on sequencing.
 *
 * Deviations: no fiemap or unwritten-extent assertion, as above. Upstream's
 * 100 iterations become G032_ITERS, because each one is ~1 MB of real RPC
 * traffic against a loopback server rather than a page-cache write; the
 * iteration count is a knob, not a property of the bug. The background loop
 * calls sync_filesystem() on the NFS superblock, which is what syncfs(2)
 * reduces to.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/namei.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G032_ROOT	XFS_MNT "/g032"
#define G032_FILE	G032_ROOT "/file"
#define G032_SERVER	XFS_EXPORT "/g032/file"

#define G032_PREALLOC	0x10000		/* upstream's 64K falloc */
#define G032_LEN	0x100000	/* upstream's 1 MB overwrite */
#define G032_SUBOFF	0xc00		/* upstream's within-page offset */
#define G032_FILL	0xcd
#define G032_ITERS	10

struct g032_syncer {
	struct super_block	*sb;
	struct task_struct	*task;
	unsigned long		loops;
};

/*
 * syncfs(2) reduced to what it does to the superblock. The s_umount read
 * lock is not optional: sync_filesystem() opens with
 * WARN_ON(!rwsem_is_locked(&sb->s_umount)) (fs/sync.c:38), and so do two
 * places inside sync_inodes_sb() (fs/fs-writeback.c:2626 and :2803). The
 * first version of this thread called sync_filesystem() bare and the case
 * still reported PASSED while spraying three WARNs per loop into the kernel
 * log -- a reminder that a green KUnit result says nothing about what the
 * kernel logged underneath it. SYSCALL_DEFINE1(syncfs) takes the same lock
 * around the same call.
 */
static int g032_sync_fn(void *arg)
{
	struct g032_syncer *s = arg;

	while (!kthread_should_stop()) {
		down_read(&s->sb->s_umount);
		sync_filesystem(s->sb);
		up_read(&s->sb->s_umount);
		s->loops++;
		cond_resched();
	}
	return 0;
}

static void g032_remove_tree(void *unused)
{
	xfs_unlink(G032_FILE);
	xfs_rmdir(G032_ROOT);
}

/* the syncer must be stopped even if an assertion aborts the case */
static void g032_stop_syncer(void *arg)
{
	struct g032_syncer *s = arg;

	if (s->task) {
		kthread_stop(s->task);
		s->task = NULL;
	}
}

static void writeback_survives_a_concurrent_sync(struct kunit *test)
{
	struct g032_syncer *s;
	struct path root;
	u8 *buf, *got;
	int iter, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G032_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g032_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s);

	/*
	 * The syncer needs the NFS superblock. Hold the path for as long as
	 * the thread runs so the mount cannot go away underneath it.
	 */
	KUNIT_ASSERT_EQ(test, kern_path(XFS_MNT, 0, &root), 0);
	s->sb = root.mnt->mnt_sb;

	s->task = kthread_run(g032_sync_fn, s, "g032-sync");
	if (IS_ERR(s->task)) {
		long e = PTR_ERR(s->task);

		s->task = NULL;
		path_put(&root);
		KUNIT_FAIL(test, "kthread_run: %ld", e);
		return;
	}
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g032_stop_syncer, s), 0);

	for (iter = 0; iter < G032_ITERS; iter++) {
		struct file *f;
		struct kstat st;
		loff_t pgoff, done;

		xfs_unlink(G032_FILE);

		f = filp_open(G032_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "iter %d: open: %ld",
				       iter, PTR_ERR(f));

		/* one sub-page write in each page of the first 64K */
		for (pgoff = 0; pgoff < G032_PREALLOC; pgoff += PAGE_SIZE) {
			loff_t pos = pgoff + G032_SUBOFF;
			u8 one = G032_FILL;

			KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, &one, 1, &pos),
					    (ssize_t)1,
					    "iter %d: seeding write at %lld",
					    iter, pgoff + G032_SUBOFF);
		}

		/* ALLOCATE over that range -- a real NFSv4.2 op, not a stub */
		err = vfs_fallocate(f, 0, 0, G032_PREALLOC);
		KUNIT_ASSERT_EQ_MSG(test, err, 0,
				    "iter %d: ALLOCATE returned %d", iter, err);

		/* overwrite well past the preallocated range, then fsync */
		memset(buf, G032_FILL, PAGE_SIZE);
		for (done = 0; done < G032_LEN; done += PAGE_SIZE) {
			loff_t pos = done;

			KUNIT_ASSERT_EQ_MSG(test,
					    kernel_write(f, buf, PAGE_SIZE, &pos),
					    (ssize_t)PAGE_SIZE,
					    "iter %d: overwrite at %lld", iter,
					    done);
		}
		KUNIT_ASSERT_EQ_MSG(test, vfs_fsync(f, 0), 0, "iter %d: fsync",
				    iter);
		filp_close(f, NULL);

		KUNIT_ASSERT_EQ(test, xfs_kstat(G032_FILE, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)G032_LEN,
				    "iter %d: size is %lld, expected %d", iter,
				    st.size, G032_LEN);

		/*
		 * The server's own bytes. Upstream cycles the mount to drop
		 * the page cache before its hexdump; reading through the
		 * export is the same guarantee without disturbing the shared
		 * fixture. Every byte must be the fill -- a lost sub-page
		 * write or a dropped overwrite shows up as a zero.
		 */
		for (done = 0; done < G032_LEN; done += PAGE_SIZE) {
			ssize_t r = xfs_read_range(G032_SERVER, got, PAGE_SIZE,
						   done);
			size_t i;

			KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)PAGE_SIZE,
					    "iter %d: server read at %lld returned %zd",
					    iter, done, r);
			for (i = 0; i < PAGE_SIZE; i++)
				if (got[i] != G032_FILL) {
					KUNIT_FAIL(test,
						   "iter %d: SERVER byte %lld is %02x, expected %02x",
						   iter, done + (loff_t)i,
						   got[i], G032_FILL);
					path_put(&root);
					return;
				}
		}
	}

	kthread_stop(s->task);
	s->task = NULL;
	path_put(&root);

	/*
	 * If the syncer never got to run, the concurrency this port exists
	 * for did not happen and a pass means much less. Report it, and fail
	 * rather than quietly claiming coverage.
	 */
	kunit_info(test, "background sync_filesystem() loops: %lu", s->loops);
	KUNIT_EXPECT_GT_MSG(test, s->loops, 0UL,
			    "the background syncer never ran -- this case did not test concurrency");
}

static int g032_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g032_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g032_cases[] = {
	KUNIT_CASE_SLOW(writeback_survives_a_concurrent_sync),
	{}
};

static struct kunit_suite g032_suite = {
	.name		= "xfstests/generic/032",
	.suite_init	= g032_suite_init,
	.suite_exit	= g032_suite_exit,
	.test_cases	= g032_cases,
};

kunit_test_suites(&g032_suite);

MODULE_DESCRIPTION("xfstests generic/032 over a loopback NFS mount");
MODULE_LICENSE("GPL");
