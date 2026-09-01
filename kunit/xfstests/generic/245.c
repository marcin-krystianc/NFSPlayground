// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/245 over a loopback NFS mount: rename onto non-empty targets.
 *
 * Upstream checks that renaming a directory onto a non-empty directory
 * fails. The port pins the errno (ENOTEMPTY over NFS) from both an empty
 * and a populated source, confirms nothing moved, and confirms the
 * positive case (empty target) still works afterwards.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G245_ROOT	XFS_MNT "/g245"

static void g245_remove_tree(void *unused)
{
	xfs_unlink(G245_ROOT "/dst/keep");
	xfs_unlink(G245_ROOT "/src/mine");
	xfs_rmdir(G245_ROOT "/src");
	xfs_rmdir(G245_ROOT "/dst");
	xfs_rmdir(G245_ROOT "/empty");
	xfs_rmdir(G245_ROOT);
}

static void nonempty_rename_targets_are_refused(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g245_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/src"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/dst"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/empty"), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G245_ROOT "/dst/keep", "k", 1), 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_rename(G245_ROOT "/src", G245_ROOT "/dst"),
			    -ENOTEMPTY,
			    "renaming onto a non-empty directory must fail ENOTEMPTY");

	/* a populated source changes nothing about the rule */
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G245_ROOT "/src/mine", "m", 1), 0);
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G245_ROOT "/src", G245_ROOT "/dst"),
			-ENOTEMPTY);

	/* nothing moved */
	KUNIT_EXPECT_TRUE(test, xfs_exists(G245_ROOT "/src/mine"));
	KUNIT_EXPECT_TRUE(test, xfs_exists(G245_ROOT "/dst/keep"));

	/* the empty target still accepts the rename */
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G245_ROOT "/src", G245_ROOT "/empty"), 0);
	KUNIT_EXPECT_TRUE(test, xfs_exists(G245_ROOT "/empty/mine"));
	KUNIT_ASSERT_EQ(test, xfs_rename(G245_ROOT "/empty", G245_ROOT "/src"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/empty"), 0);
}

static int g245_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g245_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g245_cases[] = {
	KUNIT_CASE(nonempty_rename_targets_are_refused),
	{}
};

static struct kunit_suite g245_suite = {
	.name		= "xfstests/generic/245",
	.suite_init	= g245_suite_init,
	.suite_exit	= g245_suite_exit,
	.test_cases	= g245_cases,
};

kunit_test_suites(&g245_suite);

MODULE_DESCRIPTION("xfstests generic/245 over a loopback NFS mount");
MODULE_LICENSE("GPL");
