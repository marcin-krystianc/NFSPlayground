// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/309 over a loopback NFS mount: directory times on rename-over.
 *
 * Upstream: moving a file onto an existing target must update the
 * directory's mtime and ctime. The port covers both the same-directory
 * rename-over and a cross-directory move, checking the affected parents'
 * times against the server each step.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G309_ROOT	XFS_MNT "/g309"

/* strictly-after comparison for timestamps */
static bool g309_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g309_remove_tree(void *unused)
{
	xfs_unlink(G309_ROOT "/d1/a");
	xfs_unlink(G309_ROOT "/d1/b");
	xfs_unlink(G309_ROOT "/d2/c");
	xfs_rmdir(G309_ROOT "/d1");
	xfs_rmdir(G309_ROOT "/d2");
	xfs_rmdir(G309_ROOT);
}

static void rename_over_updates_directory_times(struct kunit *test)
{
	struct kstat d1a, d1b, d2a, d2b;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g309_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_ROOT "/d1"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_ROOT "/d2"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_ROOT "/d1/a", "a", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_ROOT "/d1/b", "b", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_ROOT "/d2/c", "c", 1), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &d1a), 0);
	msleep(20);

	/* same-directory rename onto an existing target */
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G309_ROOT "/d1/a", G309_ROOT "/d1/b"), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &d1b), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g309_after(&d1b.mtime, &d1a.mtime),
			      "rename-over did not advance the directory mtime");
	KUNIT_EXPECT_TRUE_MSG(test, g309_after(&d1b.ctime, &d1a.ctime),
			      "rename-over did not advance the directory ctime");

	/* cross-directory move onto an existing target: both parents move */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &d1a), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d2", &d2a), 0);
	msleep(20);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G309_ROOT "/d1/b", G309_ROOT "/d2/c"), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &d1b), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d2", &d2b), 0);
	KUNIT_EXPECT_TRUE(test, g309_after(&d1b.mtime, &d1a.mtime));
	KUNIT_EXPECT_TRUE_MSG(test, g309_after(&d2b.mtime, &d2a.mtime),
			      "the receiving directory's mtime did not advance");
}

static int g309_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g309_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g309_cases[] = {
	KUNIT_CASE(rename_over_updates_directory_times),
	{}
};

static struct kunit_suite g309_suite = {
	.name		= "xfstests/generic/309",
	.suite_init	= g309_suite_init,
	.suite_exit	= g309_suite_exit,
	.test_cases	= g309_cases,
};

kunit_test_suites(&g309_suite);

MODULE_DESCRIPTION("xfstests generic/309 over a loopback NFS mount");
MODULE_LICENSE("GPL");
