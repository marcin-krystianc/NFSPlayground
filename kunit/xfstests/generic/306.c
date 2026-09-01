// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/306 over a loopback NFS mount: device nodes on a read-only mount.
 *
 * Upstream: opening a device node read-write on a read-only filesystem
 * must not fail with EROFS -- device access is not filesystem data. The
 * port creates a character node, remounts the client read-only, and pins
 * that the RW open is never refused with EROFS (a missing char driver in
 * this minimal kernel may legitimately yield ENXIO or EIO), while creating a
 * plain file next to it does fail EROFS.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G306_ROOT	XFS_MNT "/g306"

#define G306_NODE	G306_ROOT "/null"

static void g306_remove_tree(void *unused)
{
	xfs_remount_client(false);
	xfs_unlink(G306_NODE);
	xfs_rmdir(G306_ROOT);
}

static void device_opens_are_not_erofs(struct kunit *test)
{
	struct file *f;
	long err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G306_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g306_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mknod_chr(G306_NODE), 0);
	/* prime the dcache so the RO lookups below see the node */
	KUNIT_ASSERT_TRUE(test, xfs_exists(G306_NODE));

	err = xfs_remount_client(true);
	if (err)
		kunit_skip(test, "read-only remount unsupported here (%ld)",
			   err);

	f = filp_open(G306_NODE, O_RDWR, 0);
	err = IS_ERR(f) ? PTR_ERR(f) : 0;
	if (!err)
		filp_close(f, NULL);
	KUNIT_EXPECT_NE_MSG(test, err, (long)-EROFS,
			    "a device node RW open was refused with EROFS");
	/* this minimal kernel has no char(1,3) driver: ENXIO/EIO are fine */
	KUNIT_EXPECT_TRUE_MSG(test, err == 0 || err == -ENXIO || err == -EIO,
			      "device open: unexpected errno %ld", err);

	/* while ordinary file creation on the same mount is EROFS */
	f = filp_open(G306_ROOT "/new", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_TRUE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, PTR_ERR(f), (long)-EROFS);

	KUNIT_EXPECT_EQ(test, xfs_remount_client(false), 0);
}

static int g306_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g306_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g306_cases[] = {
	KUNIT_CASE(device_opens_are_not_erofs),
	{}
};

static struct kunit_suite g306_suite = {
	.name		= "xfstests/generic/306",
	.suite_init	= g306_suite_init,
	.suite_exit	= g306_suite_exit,
	.test_cases	= g306_cases,
};

kunit_test_suites(&g306_suite);

MODULE_DESCRIPTION("xfstests generic/306 over a loopback NFS mount");
MODULE_LICENSE("GPL");
