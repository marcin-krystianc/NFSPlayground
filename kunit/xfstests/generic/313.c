// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/313 over a loopback NFS mount: ctime and mtime on truncate.
 *
 * Upstream: truncate(2) must update ctime and mtime whether it shrinks,
 * grows, or leaves the size unchanged. Over NFS the last one diverges,
 * and the port pins the divergence: the client optimises away a SETATTR
 * whose size equals the current size, so no RPC is sent and the times do
 * NOT move -- upstream generic/313's same-size expectation cannot hold on
 * NFS by design.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G313_ROOT	XFS_MNT "/g313"

/* strictly-after comparison for timestamps */
static bool g313_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g313_remove_tree(void *unused)
{
	xfs_unlink(G313_ROOT "/f");
	xfs_rmdir(G313_ROOT);
}

static void g313_check_step(struct kunit *test, loff_t newsize,
			    struct kstat *prev, const char *what)
{
	struct kstat st;

	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_truncate(G313_ROOT "/f", newsize), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G313_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, newsize);
	KUNIT_EXPECT_TRUE_MSG(test, g313_after(&st.mtime, &prev->mtime),
			      "%s did not advance mtime", what);
	KUNIT_EXPECT_TRUE_MSG(test, g313_after(&st.ctime, &prev->ctime),
			      "%s did not advance ctime", what);
	*prev = st;
}

static void every_truncate_advances_the_times(struct kunit *test)
{
	struct kstat st;
	u8 buf[64] = { 0x11 };

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G313_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g313_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G313_ROOT "/f", buf, 64), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G313_ROOT "/f", &st), 0);

	g313_check_step(test, 16, &st, "shrinking truncate");
	g313_check_step(test, 4096, &st, "growing truncate");

	/* the NFS divergence: a same-size truncate is a client-side no-op */
	{
		struct kstat st2;

		msleep(20);
		KUNIT_ASSERT_EQ(test, xfs_truncate(G313_ROOT "/f", 4096), 0);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G313_ROOT "/f", &st2), 0);
		KUNIT_EXPECT_EQ_MSG(test, st2.mtime.tv_sec, st.mtime.tv_sec,
				    "a same-size truncate sent a SETATTR after all");
		KUNIT_EXPECT_EQ(test, st2.ctime.tv_sec, st.ctime.tv_sec);
	}
}

static int g313_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g313_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g313_cases[] = {
	KUNIT_CASE(every_truncate_advances_the_times),
	{}
};

static struct kunit_suite g313_suite = {
	.name		= "xfstests/generic/313",
	.suite_init	= g313_suite_init,
	.suite_exit	= g313_suite_exit,
	.test_cases	= g313_cases,
};

kunit_test_suites(&g313_suite);

MODULE_DESCRIPTION("xfstests generic/313 over a loopback NFS mount");
MODULE_LICENSE("GPL");
