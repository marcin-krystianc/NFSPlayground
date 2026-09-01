// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/015 over a loopback NFS mount: out-of-space behaviour.
 *
 * Upstream fills the scratch filesystem, checks df reports (nearly) no
 * free space, removes the file, and checks the free space comes back to
 * within tolerance of the starting value. The port runs against a 16 MB
 * tmpfs export: fill until the server returns ENOSPC (surfacing through
 * the client's WRITE/COMMIT path), then verify FSSTAT accounting through
 * statfs before and after the removal.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G015_ROOT	XFS_MNT "/g015"

/* write stamped 64K chunks + fsync until the server says ENOSPC */
static int g015_fill(struct kunit *test, const char *path, loff_t *written)
{
	struct file *f;
	u8 *buf;
	loff_t pos = 0;
	int err = 0, i;

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (i = 0; i < 1024; i++) {	/* 64M ceiling >> the 16M export */
		ssize_t n;

		memset(buf, 0x41 + (i % 26), 65536);
		n = kernel_write(f, buf, 65536, &pos);
		if (n < 0) {
			err = n;
			break;
		}
		err = vfs_fsync(f, 0);
		if (err)
			break;
	}
	filp_close(f, NULL);
	*written = pos;
	return err;
}

static void g015_remove_tree(void *unused)
{
	xfs_unlink(G015_ROOT "/fill");
	xfs_rmdir_settled(G015_ROOT);
}

static void filling_and_freeing_restores_the_space(struct kunit *test)
{
	struct kstatfs before, full, after;
	loff_t written = 0;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G015_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g015_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &before), 0);
	KUNIT_ASSERT_GT(test, (long long)before.f_bavail, 0LL);

	err = g015_fill(test, G015_ROOT "/fill", &written);
	KUNIT_ASSERT_EQ_MSG(test, err, -ENOSPC,
			    "filling a 16MB export ended with %d, wrote %lld",
			    err, written);
	KUNIT_EXPECT_GT(test, written, (loff_t)8 * 1024 * 1024);

	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &full), 0);
	KUNIT_EXPECT_LT_MSG(test, (long long)full.f_bavail,
			    (long long)before.f_bavail / 50,
			    "a full filesystem still reports plenty free");

	KUNIT_ASSERT_EQ(test, xfs_unlink(G015_ROOT "/fill"), 0);
	/* the server releases the space once its file cache lets go */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_wait_for_free_bytes((u64)before.f_bavail * before.f_bsize * 99 / 100),
			    0, "freed space never returned");
	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &after), 0);
	KUNIT_EXPECT_GE_MSG(test, (long long)after.f_bavail,
			    (long long)before.f_bavail * 99 / 100,
			    "free space did not come back after the removal "
			    "(before %llu, after %llu)",
			    (u64)before.f_bavail, (u64)after.f_bavail);
}

static int g015_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=8192");
	return xfstests_nfs_get();
}

static void g015_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g015_cases[] = {
	KUNIT_CASE_SLOW(filling_and_freeing_restores_the_space),
	{}
};

static struct kunit_suite g015_suite = {
	.name		= "xfstests/generic/015",
	.suite_init	= g015_suite_init,
	.suite_exit	= g015_suite_exit,
	.test_cases	= g015_cases,
};

kunit_test_suites(&g015_suite);

MODULE_DESCRIPTION("xfstests generic/015 over a loopback NFS mount");
MODULE_LICENSE("GPL");
