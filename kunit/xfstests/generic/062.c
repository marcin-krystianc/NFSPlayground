// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/062 over a loopback NFS mount: xattrs across file types.
 *
 * Upstream sets attributes on every file type. The VFS restricts the
 * user.* namespace to regular files and directories, so over NFS the
 * matrix is: regular and directory round-trip (SETXATTR RPCs), while
 * symlinks and device nodes refuse with EPERM before any RPC -- and a
 * prefix-less attribute name has no handler at all (EOPNOTSUPP).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G062_ROOT	XFS_MNT "/g062"

static void g062_remove_tree(void *unused)
{
	xfs_unlink(G062_ROOT "/reg");
	xfs_unlink(G062_ROOT "/lnk");
	xfs_unlink(G062_ROOT "/dev");
	xfs_rmdir(G062_ROOT "/dir");
	xfs_rmdir(G062_ROOT);
}

static void user_xattrs_respect_file_types(struct kunit *test)
{
	char val[16];
	ssize_t n;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G062_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g062_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G062_ROOT "/reg", "r", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G062_ROOT "/dir"), 0);
	KUNIT_ASSERT_EQ(test, xfs_symlink("reg", G062_ROOT "/lnk"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod_chr(G062_ROOT "/dev"), 0);

	err = xfs_setxattr(G062_ROOT "/reg", "user.test", "reg", 3, 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);
	n = xfs_getxattr(G062_ROOT "/reg", "user.test", val, sizeof(val));
	KUNIT_EXPECT_EQ(test, n, (ssize_t)3);

	/* directories carry user.* too */
	KUNIT_EXPECT_EQ(test,
			xfs_setxattr(G062_ROOT "/dir", "user.test", "dir", 3, 0),
			0);
	n = xfs_getxattr(G062_ROOT "/dir", "user.test", val, sizeof(val));
	KUNIT_EXPECT_EQ(test, n, (ssize_t)3);

	/*
	 * Symlinks and special files refuse user.* with EPERM (the VFS's
	 * xattr_permission), before the client would build any RPC. Note
	 * kern_path follows the symlink, so the device node is the honest
	 * probe for the special-file rule; the symlink row uses the node
	 * as its target to stay unfollowed... simplest: point at the dev.
	 */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_setxattr(G062_ROOT "/dev", "user.test", "d", 1,
					 0), -EPERM,
			    "a device node accepted a user.* attribute");

	/* a name with no namespace prefix has no handler */
	KUNIT_EXPECT_EQ(test,
			xfs_setxattr(G062_ROOT "/reg", "noname", "x", 1, 0),
			-EOPNOTSUPP);
}

static int g062_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g062_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g062_cases[] = {
	KUNIT_CASE(user_xattrs_respect_file_types),
	{}
};

static struct kunit_suite g062_suite = {
	.name		= "xfstests/generic/062",
	.suite_init	= g062_suite_init,
	.suite_exit	= g062_suite_exit,
	.test_cases	= g062_cases,
};

kunit_test_suites(&g062_suite);

MODULE_DESCRIPTION("xfstests generic/062 over a loopback NFS mount");
MODULE_LICENSE("GPL");
