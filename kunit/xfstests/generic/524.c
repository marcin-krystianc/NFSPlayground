// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/524 over a loopback NFS mount: writeback racing a
 * truncate and a rewrite of the last page.
 *
 * Upstream, 16 times on SCRATCH_MNT: truncate a file to 0, truncate it to
 * 32 MiB and fsync, pwrite all 32 MiB, then start "sync_range -w 0 0" in
 * the background while the foreground truncates one page off the end and
 * rewrites that page; then it cycles the mount. The bug was XFS writeback
 * using a stale extent mapping for the page the truncate had just dropped;
 * the pass criterion is that nothing asserts and the mount cycle works.
 *
 * Over NFS the race is nfs_writepages() building WRITE requests for the
 * dirty pages while nfs_setattr() truncates the page cache under it and
 * nfs_write_begin() dirties the last page again.
 *
 * Deviations: SCRATCH_MNT is the fixture's second mount of the export
 * (XFS_SCRATCH_MNT), cycled with xfs_scratch_umount()/xfs_scratch_mount().
 * Every step uses one open file rather than one xfs_io process each, and
 * the background sync_range is a kthread calling sync_file_range() on it. The port also checks the file after each cycle: its
 * size and its last two pages, which are the ones the race touches.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G524_FILE	XFS_SCRATCH_MNT "/g524/file"
#define G524_SERVER	XFS_EXPORT "/g524/file"
#define G524_SIZE	(32 * 1024 * 1024)
#define G524_LOOPS	16
#define G524_CHUNK	(1024 * 1024)

struct g524_sync {
	struct file		*f;
	int			err;
	struct completion	done;
};

/* xfs_io -c "sync_range -w 0 0" */
static int g524_sync_range(void *arg)
{
	struct g524_sync *s = arg;

	s->err = sync_file_range(s->f, 0, 0, SYNC_FILE_RANGE_WRITE);
	complete(&s->done);
	return 0;
}

static void g524_remove_tree(void *unused)
{
	xfs_scratch_umount();
	xfs_unlink(XFS_MNT "/g524/file");
	xfs_rmdir_settled(XFS_MNT "/g524");
}

static void g524_pwrite(struct kunit *test, struct file *f, const u8 *buf,
			loff_t off, loff_t len)
{
	while (len) {
		size_t n = min_t(loff_t, len, G524_CHUNK);
		loff_t pos = off;

		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, n, &pos),
				(ssize_t)n);
		off += n;
		len -= n;
	}
}

static void g524_check(struct kunit *test, const char *path, u8 *buf, int i)
{
	struct kstat st;
	int j;

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G524_SIZE,
			    "loop %d: %s is %lld bytes", i, path, st.size);
	KUNIT_ASSERT_EQ(test,
			xfs_read_range(path, buf, 2 * PAGE_SIZE,
				       G524_SIZE - 2 * PAGE_SIZE),
			(ssize_t)(2 * PAGE_SIZE));
	for (j = 0; j < 2 * PAGE_SIZE; j++)
		if (buf[j] != 0xcd) {
			KUNIT_FAIL(test, "loop %d: %s byte %lld is %02x",
				   i, path, (long long)(G524_SIZE - 2 * PAGE_SIZE + j),
				   buf[j]);
			return;
		}
}

static void writeback_races_truncate_of_last_page(struct kunit *test)
{
	loff_t truncsize = G524_SIZE - PAGE_SIZE;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g524_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_SCRATCH_MNT "/g524"), 0);

	/* xfs_io's pwrite pattern */
	buf = kunit_kmalloc(test, G524_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, G524_CHUNK);

	for (i = 0; i < G524_LOOPS; i++) {
		struct g524_sync s = {};
		struct task_struct *t;
		struct file *f;

		f = filp_open(G524_FILE, O_RDWR | O_CREAT, 0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "loop %d: open: %ld",
				       i, PTR_ERR(f));
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G524_SIZE), 0);
		KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
		g524_pwrite(test, f, buf, 0, G524_SIZE);

		s.f = f;
		init_completion(&s.done);
		t = kthread_run(g524_sync_range, &s, "g524-sync-range");
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
		KUNIT_EXPECT_EQ(test, xfs_ftruncate(f, truncsize), 0);
		g524_pwrite(test, f, buf, truncsize, PAGE_SIZE);
		wait_for_completion(&s.done);
		KUNIT_EXPECT_EQ_MSG(test, s.err, 0, "loop %d: sync_range: %d",
				    i, s.err);
		filp_close(f, NULL);

		/* _scratch_cycle_mount */
		KUNIT_ASSERT_EQ_MSG(test, xfs_scratch_umount(), 0,
				    "loop %d: umount", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_scratch_mount(), 0,
				    "loop %d: mount", i);

		g524_check(test, G524_FILE, buf, i);
		g524_check(test, G524_SERVER, buf, i);
		memset(buf, 0xcd, G524_CHUNK);
	}
}

static int g524_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g524_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g524_cases[] = {
	KUNIT_CASE_SLOW(writeback_races_truncate_of_last_page),
	{}
};

static struct kunit_suite g524_suite = {
	.name		= "xfstests/generic/524",
	.suite_init	= g524_suite_init,
	.suite_exit	= g524_suite_exit,
	.test_cases	= g524_cases,
};

kunit_test_suites(&g524_suite);

MODULE_DESCRIPTION("xfstests generic/524 over a loopback NFS mount");
MODULE_LICENSE("GPL");
