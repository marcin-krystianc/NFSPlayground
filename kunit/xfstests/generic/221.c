// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/221 over a loopback NFS mount: explicit utimes update ctime.
 *
 * Upstream checks that setting mtime via futimens also updates ctime.
 * Over NFS a utimes call is a SETATTR carrying the explicit timestamps;
 * the server sets ctime itself. Pinned: the explicit atime/mtime land
 * exactly, and ctime moves forward on each SETATTR rather than being
 * client-controlled.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G221_ROOT	XFS_MNT "/g221"

/* strictly-after comparison for timestamps */
static bool g221_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g221_remove_tree(void *unused)
{
	xfs_unlink(G221_ROOT "/f");
	xfs_rmdir(G221_ROOT);
}

static void utimes_sets_times_and_advances_ctime(struct kunit *test)
{
	struct kstat st0, st1, st2;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G221_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g221_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G221_ROOT "/f", "t", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_ROOT "/f", &st0), 0);

	msleep(20);	/* ensure a ctime step is observable */
	KUNIT_ASSERT_EQ(test, xfs_utimes(G221_ROOT "/f", 1000, 2000), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_ROOT "/f", &st1), 0);

	KUNIT_EXPECT_EQ_MSG(test, st1.atime.tv_sec, (time64_t)1000,
			    "explicit atime did not land");
	KUNIT_EXPECT_EQ_MSG(test, st1.mtime.tv_sec, (time64_t)2000,
			    "explicit mtime did not land");
	KUNIT_EXPECT_TRUE_MSG(test, g221_after(&st1.ctime, &st0.ctime),
			      "SETATTR(times) did not advance ctime");

	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_utimes(G221_ROOT "/f", 1000, 2000), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_ROOT "/f", &st2), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g221_after(&st2.ctime, &st1.ctime),
			      "a repeated SETATTR did not advance ctime again");
}

static int g221_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g221_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g221_cases[] = {
	KUNIT_CASE(utimes_sets_times_and_advances_ctime),
	{}
};

static struct kunit_suite g221_suite = {
	.name		= "xfstests/generic/221",
	.suite_init	= g221_suite_init,
	.suite_exit	= g221_suite_exit,
	.test_cases	= g221_cases,
};

kunit_test_suites(&g221_suite);

MODULE_DESCRIPTION("xfstests generic/221 over a loopback NFS mount");
MODULE_LICENSE("GPL");
