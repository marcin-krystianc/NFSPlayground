// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/464 over a loopback NFS mount: writes, appends and
 * non-integrity syncs racing writeback.
 *
 * Upstream runs sixteen processes for a few seconds each over a pool of
 * files, mixing three operations: rewrite a whole file, append a page to
 * one, and sync_file_range() one without waiting. The combination is
 * aimed at the race between a file's block map changing and writeback
 * using the mapping it looked up earlier. Its pass criterion is silence.
 *
 * Over NFS there is no block map, but there is a direct equivalent: the
 * client's per-inode list of dirty nfs_page requests, which a rewrite
 * coalesces into, an append extends, and a sync flushes. Getting that
 * wrong loses a write rather than corrupting a map, so the port checks
 * what upstream cannot: after everything settles, every file's size is
 * the size its last operation left it at, and every byte of it is one of
 * the patterns that were written.
 *
 * Deviations: four kthreads over eight files with bounded rounds, rather
 * than sixteen processes for five seconds; sync_file_range() becomes
 * vfs_fsync_range() with SYNC_FILE_RANGE_WRITE's meaning -- start the
 * writeback, do not wait for it -- expressed as
 * filemap_flush(), which is what that flag does.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/prandom.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G464_ROOT	XFS_MNT "/g464"
#define G464_FILES	8
#define G464_WORKERS	4
#define G464_ROUNDS	60
#define G464_BLOCK	65536
#define G464_MAXBLKS	8
#define G464_BYTE	0x64

struct g464_worker {
	int			id;
	int			err;
	unsigned long		ops;
	struct completion	done;
};

static void g464_path(char *buf, size_t size, int i)
{
	snprintf(buf, size, G464_ROOT "/%d", i);
}

static int g464_mixed_ops(void *arg)
{
	struct g464_worker *w = arg;
	struct rnd_state rnd;
	char path[64];
	u8 *buf;
	int i;

	buf = kmalloc(G464_BLOCK, GFP_KERNEL);
	if (!buf) {
		w->err = -ENOMEM;
		complete(&w->done);
		return 0;
	}
	memset(buf, G464_BYTE, G464_BLOCK);
	prandom_seed_state(&rnd, 464 + w->id);

	for (i = 0; i < G464_ROUNDS; i++) {
		unsigned int r = prandom_u32_state(&rnd);
		struct file *f;
		loff_t pos;
		int blocks;

		g464_path(path, sizeof(path), r % G464_FILES);
		f = filp_open(path, O_RDWR | O_CREAT, 0644);
		if (IS_ERR(f)) {
			w->err = PTR_ERR(f);
			break;
		}

		switch (r % 3) {
		case 0:		/* rewrite the file */
			blocks = 1 + (prandom_u32_state(&rnd) % G464_MAXBLKS);
			pos = 0;
			while (blocks--) {
				ssize_t n = kernel_write(f, buf, G464_BLOCK,
							 &pos);

				if (n != G464_BLOCK) {
					w->err = n < 0 ? (int)n : -EIO;
					break;
				}
			}
			break;
		case 1:		/* append a page */
			pos = i_size_read(file_inode(f));
			if (kernel_write(f, buf, PAGE_SIZE, &pos) !=
			    (ssize_t)PAGE_SIZE)
				w->err = -EIO;
			break;
		default:	/* start writeback without waiting */
			filemap_flush(f->f_mapping);
			break;
		}
		w->ops++;
		filp_close(f, NULL);
		if (w->err)
			break;
		cond_resched();
	}
	kfree(buf);
	complete(&w->done);
	return 0;
}

static void g464_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < G464_FILES; i++) {
		g464_path(path, sizeof(path), i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G464_ROOT);
}

static void mixed_writes_and_syncs_lose_nothing(struct kunit *test)
{
	struct g464_worker workers[G464_WORKERS];
	struct task_struct *t;
	char path[64];
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G464_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g464_remove_tree, NULL),
			0);

	for (i = 0; i < G464_WORKERS; i++) {
		workers[i] = (struct g464_worker){ .id = i };
		init_completion(&workers[i].done);
		t = kthread_run(g464_mixed_ops, &workers[i], "g464-worker%d",
				i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
	}

	for (i = 0; i < G464_WORKERS; i++) {
		wait_for_completion(&workers[i].done);
		KUNIT_EXPECT_EQ_MSG(test, workers[i].err, 0,
				    "worker %d failed with %d", i,
				    workers[i].err);
		KUNIT_EXPECT_GT_MSG(test, workers[i].ops, 0UL,
				    "worker %d did nothing", i);
	}

	/* every byte of every file is one the workers wrote */
	buf = kunit_kmalloc(test, G464_BLOCK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G464_FILES; i++) {
		struct kstat st;
		loff_t off;

		g464_path(path, sizeof(path), i);
		if (!xfs_exists(path))
			continue;
		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		for (off = 0; off < st.size; off += G464_BLOCK) {
			size_t want = min_t(loff_t, st.size - off, G464_BLOCK);
			ssize_t n = xfs_read_range(path, buf, want, off);
			int j;

			KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)want,
					    "file %d: read at %lld returned %zd",
					    i, off, n);
			for (j = 0; j < want; j++)
				if (buf[j] != G464_BYTE) {
					KUNIT_FAIL(test,
						   "file %d: byte %lld is %02x, expected %02x",
						   i, off + j, buf[j],
						   G464_BYTE);
					return;
				}
		}
	}
}

static int g464_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g464_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g464_cases[] = {
	KUNIT_CASE_SLOW(mixed_writes_and_syncs_lose_nothing),
	{}
};

static struct kunit_suite g464_suite = {
	.name		= "xfstests/generic/464",
	.suite_init	= g464_suite_init,
	.suite_exit	= g464_suite_exit,
	.test_cases	= g464_cases,
};

kunit_test_suites(&g464_suite);

MODULE_DESCRIPTION("xfstests generic/464 over a loopback NFS mount");
MODULE_LICENSE("GPL");
