// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/755 over a loopback NFS mount: unlinking one hard link
 * moves the other name's ctime.
 *
 * Upstream creates a file, hard-links it, samples the file's ctime,
 * sleeps, unlinks the link and requires the file's ctime to have moved.
 * Removing a name changes the inode's link count, and a link count change
 * is a ctime change (btrfs commit 3bc2ac2f8f0b, "btrfs: update target
 * inode's ctime on unlink").
 *
 * Over NFS the REMOVE goes to the server, which updates ctime there; the
 * client then has to notice, on a name it did not touch. That is the
 * same shape as generic/378's mode check and generic/236's link check,
 * but from the other direction -- this is the name that stays.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G755_ROOT	XFS_MNT "/g755"
#define G755_FILE	G755_ROOT "/unlink-ctime1"
#define G755_LINK	G755_ROOT "/unlink-ctime2"

static void g755_remove_tree(void *unused)
{
	xfs_unlink(G755_LINK);
	xfs_unlink(G755_FILE);
	xfs_rmdir_settled(G755_ROOT);
}

static bool g755_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void unlinking_a_link_moves_the_targets_ctime(struct kunit *test)
{
	struct kstat before, after;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G755_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g755_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G755_FILE, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_link(G755_FILE, G755_LINK), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G755_FILE, &before), 0);
	KUNIT_ASSERT_EQ_MSG(test, before.nlink, 2U, "nlink is %u",
			    before.nlink);

	msleep(20);	/* upstream's sleep 2, scaled */
	KUNIT_ASSERT_EQ(test, xfs_unlink(G755_LINK), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G755_FILE, &after), 0);
	KUNIT_EXPECT_EQ_MSG(test, after.nlink, 1U, "nlink is %u after unlink",
			    after.nlink);
	KUNIT_EXPECT_TRUE_MSG(test, g755_after(&after.ctime, &before.ctime),
			      "ctime did not change after unlinking the other name: %lld.%09ld -> %lld.%09ld",
			      before.ctime.tv_sec, before.ctime.tv_nsec,
			      after.ctime.tv_sec, after.ctime.tv_nsec);
}

static int g755_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g755_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g755_cases[] = {
	KUNIT_CASE(unlinking_a_link_moves_the_targets_ctime),
	{}
};

static struct kunit_suite g755_suite = {
	.name		= "xfstests/generic/755",
	.suite_init	= g755_suite_init,
	.suite_exit	= g755_suite_exit,
	.test_cases	= g755_cases,
};

kunit_test_suites(&g755_suite);

MODULE_DESCRIPTION("xfstests generic/755 over a loopback NFS mount");
MODULE_LICENSE("GPL");
