// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/204 over a loopback NFS mount: ENOSPC with many small files.
 *
 * Upstream hits ENOSPC via a stream of small files, exercising the
 * flushing that has to happen when space runs out mid-stream. The port
 * creates 8 KB files until the 16 MB export refuses, requires that a
 * healthy number fit before that, then removes everything and expects
 * the directory to empty cleanly.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G204_ROOT	XFS_MNT "/g204"

static void g204_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < 4096; i++) {
		snprintf(buf, sizeof(buf), G204_ROOT "/s%04d", i);
		if (xfs_unlink(buf) == -ENOENT)
			break;
	}
	xfs_rmdir_settled(G204_ROOT);
}

static void small_file_stream_hits_enospc_cleanly(struct kunit *test)
{
	char name[64];
	u8 *buf;
	int i, created = 0, err = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G204_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g204_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x33, 8192);

	for (i = 0; i < 4096; i++) {
		struct file *f;
		loff_t pos = 0;
		ssize_t n;

		snprintf(name, sizeof(name), G204_ROOT "/s%04d", i);
		f = filp_open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (IS_ERR(f)) {
			err = PTR_ERR(f);
			break;
		}
		n = kernel_write(f, buf, 8192, &pos);
		if (n == 8192)
			err = vfs_fsync(f, 0);
		else
			err = (n < 0) ? n : -EIO;
		filp_close(f, NULL);
		if (err)
			break;
		created++;
	}

	KUNIT_EXPECT_EQ_MSG(test, err, -ENOSPC,
			    "expected the stream to end in ENOSPC, got %d after %d files",
			    err, created);
	KUNIT_EXPECT_GT_MSG(test, created, 1000,
			    "only %d 8KB files fit in a 16MB export", created);

	for (i = 0; i <= created; i++) {
		snprintf(name, sizeof(name), G204_ROOT "/s%04d", i);
		if (xfs_unlink(name) == -ENOENT)
			break;
	}
	KUNIT_EXPECT_EQ(test, xfs_wait_for_free_bytes(4 * 1024 * 1024), 0);
	KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir_settled(G204_ROOT), 0,
			    "the directory would not empty after ENOSPC churn");
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G204_ROOT), 0);	/* for the action */
}

static int g204_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=8192");
	return xfstests_nfs_get();
}

static void g204_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g204_cases[] = {
	KUNIT_CASE_SLOW(small_file_stream_hits_enospc_cleanly),
	{}
};

static struct kunit_suite g204_suite = {
	.name		= "xfstests/generic/204",
	.suite_init	= g204_suite_init,
	.suite_exit	= g204_suite_exit,
	.test_cases	= g204_cases,
};

kunit_test_suites(&g204_suite);

MODULE_DESCRIPTION("xfstests generic/204 over a loopback NFS mount");
MODULE_LICENSE("GPL");
