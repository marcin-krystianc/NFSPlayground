// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/294 over a loopback NFS mount: EEXIST on a read-only filesystem.
 *
 * Upstream's point: asking to create an already-existing name on a
 * read-only filesystem must report EEXIST, not EROFS. Over NFS the
 * answer turns out to be two-regime, and this port pins both:
 *
 *  - name in the dcache: the VFS sees the positive dentry, EEXIST wins;
 *  - cold dcache: nfs_lookup() deliberately skips the server round-trip
 *    for LOOKUP_CREATE|LOOKUP_EXCL lookups (it is "about to create
 *    anyway"), the dentry comes back negative, and the read-only check
 *    fires first: EROFS -- even though the name exists on the server.
 *
 * So on NFS, upstream generic/294's expectation only holds for cached
 * names. Found empirically here: adding a lookup before the create
 * flipped the errno from EROFS to EEXIST.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G294_ROOT	XFS_MNT "/g294"

static void g294_remove_tree(void *unused)
{
	xfs_remount_client(false);
	xfs_unlink(G294_ROOT "/file");
	xfs_unlink(G294_ROOT "/node");
	xfs_unlink(G294_ROOT "/link");
	xfs_rmdir(G294_ROOT "/dir");
	xfs_rmdir(G294_ROOT);
}

static void eexist_beats_erofs(struct kunit *test)
{
	struct file *f;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G294_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g294_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G294_ROOT "/file", "f", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G294_ROOT "/dir"), 0);
	KUNIT_ASSERT_EQ(test, xfs_symlink("file", G294_ROOT "/link"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod_chr(G294_ROOT "/node"), 0);

	err = xfs_remount_client(true);
	if (err)
		kunit_skip(test, "read-only remount unsupported here (%d)", err);

	/*
	 * Regime 1 -- cold dcache: the exclusive-create lookup optimisation
	 * means the client never asks the server, so EROFS wins even though
	 * every one of these names exists.
	 */
	KUNIT_EXPECT_EQ_MSG(test, xfs_symlink("x", G294_ROOT "/link"), -EROFS,
			    "cold-dcache exclusive create should be EROFS on NFS");
	KUNIT_EXPECT_EQ(test, xfs_mknod_chr(G294_ROOT "/node"), -EROFS);

	/*
	 * Regime 2 -- primed dcache: a plain lookup fetches the positive
	 * dentry from the server, and EEXIST beats EROFS as upstream
	 * expects on local filesystems.
	 */
	KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(G294_ROOT "/file"),
			      "existing file invisible after RO remount");
	KUNIT_ASSERT_TRUE(test, xfs_exists(G294_ROOT "/dir"));
	KUNIT_ASSERT_TRUE(test, xfs_exists(G294_ROOT "/link"));
	KUNIT_ASSERT_TRUE(test, xfs_exists(G294_ROOT "/node"));

	f = filp_open(G294_ROOT "/file", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_TRUE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, PTR_ERR(f), (long)-EEXIST);
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G294_ROOT "/dir"), -EEXIST);
	KUNIT_EXPECT_EQ_MSG(test, xfs_symlink("x", G294_ROOT "/link"), -EEXIST,
			    "a primed dcache should flip the errno to EEXIST");
	KUNIT_EXPECT_EQ(test, xfs_mknod_chr(G294_ROOT "/node"), -EEXIST);

	/* fresh names on the same mount: EROFS */
	f = filp_open(G294_ROOT "/new", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_TRUE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, PTR_ERR(f), (long)-EROFS);
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G294_ROOT "/newdir"), -EROFS);

	KUNIT_EXPECT_EQ(test, xfs_remount_client(false), 0);
}

static int g294_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g294_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g294_cases[] = {
	KUNIT_CASE(eexist_beats_erofs),
	{}
};

static struct kunit_suite g294_suite = {
	.name		= "xfstests/generic/294",
	.suite_init	= g294_suite_init,
	.suite_exit	= g294_suite_exit,
	.test_cases	= g294_cases,
};

kunit_test_suites(&g294_suite);

MODULE_DESCRIPTION("xfstests generic/294 over a loopback NFS mount");
MODULE_LICENSE("GPL");
