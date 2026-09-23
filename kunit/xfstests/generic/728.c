// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/728 over a loopback NFS mount: ctime after an xattr
 * operation.
 *
 * This one was written for the NFS client. Upstream's own comment: "Test
 * a bug where the NFS client wasn't sending a post-op GETATTR to the
 * server after setting an xattr, resulting in `stat` reporting a stale
 * ctime." It samples ctime, sleeps, sets user.foobar, and requires ctime
 * to have moved; then the same for removing it.
 *
 * The detail that makes this test work is that the stat must be an
 * ordinary one. Everywhere else in these ports attributes are read with
 * AT_STATX_FORCE_SYNC, which forces a fresh GETATTR and would hide
 * exactly the bug under test -- a client that never invalidated its
 * cached attributes would still look correct. So this port uses a plain
 * vfs_getattr(), the same thing stat(1) does, and lets the client answer
 * from its cache if it thinks it may.
 *
 * Deviations: upstream's "sleep 2" (for filesystems whose ctime
 * granularity is two seconds) is 20 ms here; the server is tmpfs, whose
 * timestamps are nanosecond-granular, and the comparison is on the full
 * timespec.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/delay.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G728_ROOT	XFS_MNT "/g728"
#define G728_FILE	G728_ROOT "/testfile"

static void g728_remove_tree(void *unused)
{
	xfs_unlink(G728_FILE);
	xfs_rmdir_settled(G728_ROOT);
}

/* stat(1)'s getattr: no FORCE_SYNC, so the client may answer from cache */
static int g728_stat(const char *path, struct kstat *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_getattr(&p, st, STATX_BASIC_STATS, 0);
	path_put(&p);
	return err;
}

static bool g728_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void xattr_operations_move_the_visible_ctime(struct kunit *test)
{
	struct kstat before, after;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G728_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g728_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G728_FILE, "", 0), 0);

	/* setxattr */
	KUNIT_ASSERT_EQ(test, g728_stat(G728_FILE, &before), 0);
	msleep(20);
	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G728_FILE, "user.foobar", "123", 3, 0), 0);
	KUNIT_ASSERT_EQ(test, g728_stat(G728_FILE, &after), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g728_after(&after.ctime, &before.ctime),
			      "ctime did not change after setxattr: %lld.%09ld -> %lld.%09ld",
			      before.ctime.tv_sec, before.ctime.tv_nsec,
			      after.ctime.tv_sec, after.ctime.tv_nsec);

	/* removexattr */
	before = after;
	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G728_FILE, "user.foobar"), 0);
	KUNIT_ASSERT_EQ(test, g728_stat(G728_FILE, &after), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g728_after(&after.ctime, &before.ctime),
			      "ctime did not change after removexattr: %lld.%09ld -> %lld.%09ld",
			      before.ctime.tv_sec, before.ctime.tv_nsec,
			      after.ctime.tv_sec, after.ctime.tv_nsec);
}

static int g728_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g728_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g728_cases[] = {
	KUNIT_CASE(xattr_operations_move_the_visible_ctime),
	{}
};

static struct kunit_suite g728_suite = {
	.name		= "xfstests/generic/728",
	.suite_init	= g728_suite_init,
	.suite_exit	= g728_suite_exit,
	.test_cases	= g728_cases,
};

kunit_test_suites(&g728_suite);

MODULE_DESCRIPTION("xfstests generic/728 over a loopback NFS mount");
MODULE_LICENSE("GPL");
