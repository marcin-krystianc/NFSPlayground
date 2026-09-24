// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/245 over a loopback NFS mount: rename onto non-empty targets.
 *
 * Upstream (from a bug report and testcase by Vlado Plaga):
 *
 *	mkdir test-mv test-mv/aa test-mv/ab
 *	touch test-mv/aa/1
 *	mkdir test-mv/ab/aa
 *	touch test-mv/ab/aa/2
 *	mv test-mv/ab/aa/ test-mv
 *
 * mv turns the last line into rename("test-mv/ab/aa", "test-mv/aa"): a
 * non-empty directory moved to another directory, onto a non-empty
 * directory of the same name. rename(2) allows either EEXIST or ENOTEMPTY
 * for that, and the golden output accepts either. Nothing may move.
 *
 * A second case, not in upstream, is the same rule within one directory:
 * an empty and then a populated source onto a non-empty target, and the
 * positive case (an empty target) still working afterwards.
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
	xfs_settle_fput();
	xfs_unlink(G245_ROOT "/test-mv/aa/1");
	xfs_unlink(G245_ROOT "/test-mv/ab/aa/2");
	xfs_rmdir(G245_ROOT "/test-mv/ab/aa");
	xfs_rmdir(G245_ROOT "/test-mv/aa");
	xfs_rmdir(G245_ROOT "/test-mv/ab");
	xfs_rmdir(G245_ROOT "/test-mv");
	xfs_unlink(G245_ROOT "/dst/keep");
	xfs_unlink(G245_ROOT "/src/mine");
	xfs_rmdir(G245_ROOT "/src");
	xfs_rmdir(G245_ROOT "/dst");
	xfs_rmdir(G245_ROOT "/empty");
	xfs_rmdir_settled(G245_ROOT);
}

static void moving_a_directory_onto_a_nonempty_namesake_fails(struct kunit *test)
{
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g245_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/test-mv"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/test-mv/aa"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/test-mv/ab"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G245_ROOT "/test-mv/aa/1",
						 "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G245_ROOT "/test-mv/ab/aa"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G245_ROOT "/test-mv/ab/aa/2",
						 "", 0), 0);

	/* mv test-mv/ab/aa/ test-mv */
	err = xfs_rename(G245_ROOT "/test-mv/ab/aa", G245_ROOT "/test-mv/aa");
	KUNIT_EXPECT_TRUE_MSG(test, err == -EEXIST || err == -ENOTEMPTY,
			      "mv: cannot overwrite 'test-mv/aa': got %d, expected EEXIST or ENOTEMPTY",
			      err);
	KUNIT_EXPECT_TRUE(test, xfs_exists(G245_ROOT "/test-mv/aa/1"));
	KUNIT_EXPECT_TRUE(test, xfs_exists(G245_ROOT "/test-mv/ab/aa/2"));
}

/* not in upstream: the same rule within one directory */
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
	KUNIT_CASE(moving_a_directory_onto_a_nonempty_namesake_fails),
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
