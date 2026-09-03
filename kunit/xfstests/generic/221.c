// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/221 over a loopback NFS mount: ctime on a partial utimes.
 *
 * Upstream's src/t_futimens is one case, and the omission is the point:
 *
 *	struct timespec t[2] = { { 1000000000, 0 }, { 0, UTIME_OMIT } };
 *	futimens(fd, t);
 *
 * atime is set explicitly, mtime is left alone, and ctime must still move.
 * A filesystem that keys "did anything change?" off the mtime request
 * prints "failed to update ctime!" here -- the bug this test came from.
 *
 * Over NFS the SETATTR carries time_access as SET_TO_CLIENT_TIME4 and no
 * time_modify at all, so the server has to notice the operation and stamp
 * ctime itself. The port checks the same thing, plus the two facts that make
 * the check meaningful: the explicit atime landed and the omitted mtime did
 * not move.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>		/* UTIME_OMIT */
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G221_ROOT	XFS_MNT "/g221"
#define G221_FILE	G221_ROOT "/f"

/* upstream's t[0]: one second past the 2001 epoch tick */
#define G221_ATIME	1000000000

/* strictly-after comparison for timestamps */
static bool g221_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g221_remove_tree(void *unused)
{
	xfs_unlink(G221_FILE);
	xfs_rmdir(G221_ROOT);
}

static void utimes_with_mtime_omitted_still_moves_ctime(struct kunit *test)
{
	struct timespec64 t[2] = {
		{ .tv_sec = G221_ATIME,	.tv_nsec = 0 },
		{ .tv_sec = 0,		.tv_nsec = UTIME_OMIT },
	};
	struct kstat st0, st1;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G221_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g221_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G221_FILE, "t", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_FILE, &st0), 0);

	msleep(20);	/* upstream's sleep(1), scaled: a ctime step must be visible */
	KUNIT_ASSERT_EQ(test, xfs_utimes_raw(G221_FILE, t), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_FILE, &st1), 0);

	/* the upstream assertion */
	KUNIT_EXPECT_TRUE_MSG(test, g221_after(&st1.ctime, &st0.ctime),
			      "failed to update ctime: a utimes with mtime UTIME_OMIT left ctime at %lld.%09ld",
			      (long long)st1.ctime.tv_sec, st1.ctime.tv_nsec);

	/* and the omission really was an omission */
	KUNIT_EXPECT_EQ_MSG(test, st1.atime.tv_sec, (time64_t)G221_ATIME,
			    "the explicit atime did not land (%lld)",
			    (long long)st1.atime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, st1.mtime.tv_sec, st0.mtime.tv_sec,
			    "UTIME_OMIT changed mtime from %lld to %lld",
			    (long long)st0.mtime.tv_sec,
			    (long long)st1.mtime.tv_sec);
}

/*
 * The mirror image, and the reason the omitted form can regress on its own:
 * with both times given, ctime must move too.
 */
static void utimes_with_both_times_moves_ctime(struct kunit *test)
{
	struct kstat st0, st1;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G221_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g221_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G221_FILE, "t", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_FILE, &st0), 0);

	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_utimes(G221_FILE, 1000, 2000), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G221_FILE, &st1), 0);

	KUNIT_EXPECT_EQ_MSG(test, st1.atime.tv_sec, (time64_t)1000,
			    "explicit atime did not land");
	KUNIT_EXPECT_EQ_MSG(test, st1.mtime.tv_sec, (time64_t)2000,
			    "explicit mtime did not land");
	KUNIT_EXPECT_TRUE_MSG(test, g221_after(&st1.ctime, &st0.ctime),
			      "SETATTR(times) did not advance ctime");
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
	KUNIT_CASE(utimes_with_mtime_omitted_still_moves_ctime),
	KUNIT_CASE(utimes_with_both_times_moves_ctime),
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
