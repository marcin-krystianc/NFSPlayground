// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/131 over a loopback NFS mount: POSIX advisory locks.
 *
 * Upstream drives fcntl advisory locks through a client/server pair of
 * processes. Over NFSv4 every lock is protocol state: LOCK/LOCKT/LOCKU
 * RPCs against server-side lockowners, no lockd involved. The port uses
 * two open files with two distinct lockowners: exclusive versus shared
 * conflicts, disjoint ranges coexisting, unlock releasing the range for
 * the waiter, and shared locks stacking.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>

#include "xfstests_nfs_fixture.h"

#define G131_ROOT	XFS_MNT "/g131"

static int g131_a, g131_b;	/* distinct lockowner cookies */

static void g131_remove_tree(void *unused)
{
	xfs_unlink(G131_ROOT "/lock");
	xfs_rmdir(G131_ROOT);
}

static void two_lockowners_conflict_correctly(struct kunit *test)
{
	struct file *fa, *fb;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G131_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g131_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G131_ROOT "/lock", "lockme", 6), 0);

	fa = filp_open(G131_ROOT "/lock", O_RDWR, 0);
	fb = filp_open(G131_ROOT "/lock", O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fa));
	KUNIT_ASSERT_FALSE(test, IS_ERR(fb));

	/* A takes an exclusive lock on [0,99] */
	KUNIT_ASSERT_EQ(test,
			xfs_posix_lock(fa, F_WRLCK, 0, 99, &g131_a, false), 0);

	/* B cannot get shared or exclusive on the same range */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_posix_lock(fb, F_RDLCK, 0, 99, &g131_b, false),
			    -EAGAIN, "a shared lock crossed an exclusive one");
	KUNIT_EXPECT_EQ(test,
			xfs_posix_lock(fb, F_WRLCK, 50, 149, &g131_b, false),
			-EAGAIN);

	/* but a disjoint exclusive range is fine */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_posix_lock(fb, F_WRLCK, 100, 199, &g131_b, false),
			    0, "disjoint ranges must not conflict");

	/* A unlocks; B can now lock the freed range */
	KUNIT_ASSERT_EQ(test,
			xfs_posix_lock(fa, F_UNLCK, 0, 99, &g131_a, false), 0);
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_posix_lock(fb, F_RDLCK, 0, 99, &g131_b, false),
			    0, "the unlocked range is still refused");

	/* shared locks stack: A can read-lock what B read-locked */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_posix_lock(fa, F_RDLCK, 0, 99, &g131_a, false),
			    0, "two shared locks did not coexist");
	/* ...but not write-lock it */
	KUNIT_EXPECT_EQ(test,
			xfs_posix_lock(fa, F_WRLCK, 0, 99, &g131_a, false),
			-EAGAIN);

	/* drop everything */
	KUNIT_ASSERT_EQ(test,
			xfs_posix_lock(fa, F_UNLCK, 0, 199, &g131_a, false), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_posix_lock(fb, F_UNLCK, 0, 199, &g131_b, false), 0);

	filp_close(fa, NULL);
	filp_close(fb, NULL);
}

static int g131_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g131_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g131_cases[] = {
	KUNIT_CASE(two_lockowners_conflict_correctly),
	{}
};

static struct kunit_suite g131_suite = {
	.name		= "xfstests/generic/131",
	.suite_init	= g131_suite_init,
	.suite_exit	= g131_suite_exit,
	.test_cases	= g131_cases,
};

kunit_test_suites(&g131_suite);

MODULE_DESCRIPTION("xfstests generic/131 over a loopback NFS mount");
MODULE_LICENSE("GPL");
