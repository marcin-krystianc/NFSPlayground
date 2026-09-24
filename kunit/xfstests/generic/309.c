// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/309 over a loopback NFS mount: directory times on a
 * move onto an existing name.
 *
 * Upstream creates testdir_309/testfile and, beside the directory,
 * testfile.309; records the directory's mtime and ctime (stat %Y and %Z,
 * whole seconds); sleeps one second; moves testfile.309 onto
 * testdir_309/testfile; and requires both of the directory's times to have
 * changed.
 *
 * Not in upstream: a same-directory rename onto an existing name must also
 * advance that directory's mtime and ctime.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G309_ROOT	XFS_MNT "/g309"
#define G309_DIR	G309_ROOT "/testdir_309"

/* strictly-after comparison for timestamps */
static bool g309_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g309_remove_tree(void *unused)
{
	xfs_unlink(G309_DIR "/testfile");
	xfs_unlink(G309_ROOT "/testfile.309");
	xfs_unlink(G309_ROOT "/d1/a");
	xfs_unlink(G309_ROOT "/d1/b");
	xfs_rmdir(G309_DIR);
	xfs_rmdir(G309_ROOT "/d1");
	xfs_rmdir(G309_ROOT);
}

static void rename_over_updates_directory_times(struct kunit *test)
{
	struct kstat t1, t2;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g309_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_DIR "/testfile", NULL, 0),
			0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G309_ROOT "/testfile.309", NULL, 0),
			0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_DIR, &t1), 0);
	ssleep(1);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G309_ROOT "/testfile.309",
				   G309_DIR "/testfile"), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_DIR, &t2), 0);
	KUNIT_EXPECT_NE_MSG(test, t1.mtime.tv_sec, t2.mtime.tv_sec,
			    "mtime not updated");
	KUNIT_EXPECT_NE_MSG(test, t1.ctime.tv_sec, t2.ctime.tv_sec,
			    "ctime not updated");

	/* not upstream: same-directory rename onto an existing name */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G309_ROOT "/d1"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_ROOT "/d1/a", "a", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G309_ROOT "/d1/b", "b", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &t1), 0);
	msleep(20);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G309_ROOT "/d1/a", G309_ROOT "/d1/b"), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G309_ROOT "/d1", &t2), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g309_after(&t2.mtime, &t1.mtime),
			      "rename-over did not advance the directory mtime");
	KUNIT_EXPECT_TRUE_MSG(test, g309_after(&t2.ctime, &t1.ctime),
			      "rename-over did not advance the directory ctime");
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
