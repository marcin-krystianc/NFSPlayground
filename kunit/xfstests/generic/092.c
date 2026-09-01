// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/092 over a loopback NFS mount: fallocate KEEP_SIZE.
 *
 * Upstream 092 (and 086/315) preallocate with FALLOC_FL_KEEP_SIZE and
 * check size/space accounting. nfs42_fallocate() accepts KEEP_SIZE only
 * in combination with PUNCH_HOLE; bare KEEP_SIZE preallocation has no
 * ALLOCATE mapping, so xfstests _notruns. Pinned here: bare KEEP_SIZE and
 * PUNCH_HOLE-without-KEEP_SIZE both refuse with EOPNOTSUPP, while the two
 * supported modes work.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G092_ROOT	XFS_MNT "/g092"

static void g092_remove_tree(void *unused)
{
	xfs_unlink(G092_ROOT "/f");
	xfs_rmdir(G092_ROOT);
}

static void keep_size_needs_punch_and_punch_needs_keep_size(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G092_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g092_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G092_ROOT "/f", "x", 1), 0);

	f = filp_open(G092_ROOT "/f", O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	err = vfs_fallocate(f, FALLOC_FL_KEEP_SIZE, 0, 65536);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "bare KEEP_SIZE preallocation: expected EOPNOTSUPP, got %d",
			    err);

	err = vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE, 0, 4096);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "PUNCH_HOLE without KEEP_SIZE must be refused (VFS or NFS), got %d",
			    err);

	/* the two supported modes still work */
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 0, 8192), 0);
	KUNIT_EXPECT_EQ(test,
			vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				      0, 4096), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G092_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)8192,
			    "ALLOCATE(0) should have extended the file to 8192");
}

static int g092_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g092_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g092_cases[] = {
	KUNIT_CASE(keep_size_needs_punch_and_punch_needs_keep_size),
	{}
};

static struct kunit_suite g092_suite = {
	.name		= "xfstests/generic/092",
	.suite_init	= g092_suite_init,
	.suite_exit	= g092_suite_exit,
	.test_cases	= g092_cases,
};

kunit_test_suites(&g092_suite);

MODULE_DESCRIPTION("xfstests generic/092 over a loopback NFS mount");
MODULE_LICENSE("GPL");
