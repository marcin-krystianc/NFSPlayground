// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/236 over a loopback NFS mount: hard links update ctime.
 *
 * Upstream: creating a hard link must update the inode's ctime (a btrfs
 * regression once broke this). Over NFS the LINK reply's post-op
 * attributes carry the new ctime; the port checks it through both the
 * original and the new name, and that unlink advances it again.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G236_ROOT	XFS_MNT "/g236"

/* strictly-after comparison for timestamps */
static bool g236_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g236_remove_tree(void *unused)
{
	xfs_unlink(G236_ROOT "/a");
	xfs_unlink(G236_ROOT "/b");
	xfs_rmdir(G236_ROOT);
}

static void linking_advances_the_inode_ctime(struct kunit *test)
{
	struct kstat st0, st1, st2;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G236_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g236_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G236_ROOT "/a", "l", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G236_ROOT "/a", &st0), 0);

	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_link(G236_ROOT "/a", G236_ROOT "/b"), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G236_ROOT "/a", &st1), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g236_after(&st1.ctime, &st0.ctime),
			      "LINK did not advance the inode ctime");
	KUNIT_EXPECT_EQ(test, st1.nlink, 2U);

	/* the second name shows the same inode state */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G236_ROOT "/b", &st2), 0);
	KUNIT_EXPECT_EQ(test, st2.ino, st1.ino);
	KUNIT_EXPECT_EQ(test, st2.ctime.tv_sec, st1.ctime.tv_sec);

	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_unlink(G236_ROOT "/b"), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G236_ROOT "/a", &st2), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g236_after(&st2.ctime, &st1.ctime),
			      "REMOVE of a link did not advance ctime");
	KUNIT_EXPECT_EQ(test, st2.nlink, 1U);
}

static int g236_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g236_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g236_cases[] = {
	KUNIT_CASE(linking_advances_the_inode_ctime),
	{}
};

static struct kunit_suite g236_suite = {
	.name		= "xfstests/generic/236",
	.suite_init	= g236_suite_init,
	.suite_exit	= g236_suite_exit,
	.test_cases	= g236_cases,
};

kunit_test_suites(&g236_suite);

MODULE_DESCRIPTION("xfstests generic/236 over a loopback NFS mount");
MODULE_LICENSE("GPL");
