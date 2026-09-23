// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/258 over a loopback NFS mount: timestamps before the
 * epoch.
 *
 * Upstream touches a file to 1 Jan 1960 01:01, requires stat to report a
 * negative seconds-since-epoch (-315593940), cycles the mount to drop any
 * cached value, and requires the same negative number to come back. It
 * was written for ext2/3/4 sign-extending on read.
 *
 * Over NFS both halves are on the wire: SETATTR has to encode a negative
 * nfstime4 seconds field (signed int64 in RFC 7530), the server has to
 * store it, and GETATTR has to decode it back without turning it into a
 * large positive number. _require_negative_timestamps notruns on NFSv2
 * and v3, whose timestamps are unsigned 32-bit, and passes from v4 on,
 * which is what the fixture mounts.
 *
 * The port splits upstream's single sequence in two, because measuring it
 * turned up a behaviour the shell test cannot see:
 *
 *  - On a file the client has opened, knfsd hands out a write delegation,
 *    and nfs_setattr() then applies ATTR_MTIME_SET/ATTR_ATIME_SET locally
 *    via nfs_set_timestamps_to_ts() and clears the bits -- no SETATTR is
 *    sent at all (fs/nfs/inode.c). The client reports the new timestamps,
 *    the server still holds the old ones, and upstream's mount cycle is
 *    what would reconcile them. That is the first case: it checks what
 *    upstream checks, on the client.
 *  - On a file the client has never opened there is no delegation, so the
 *    SETATTR goes to the server. That is the second case, and it is the
 *    one that actually proves a negative time survives the round trip:
 *    the value is read back both through the client and from the server's
 *    own copy through the tmpfs export.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/time64.h>

#include "xfstests_nfs_fixture.h"

#define G258_ROOT	XFS_MNT "/g258"
#define G258_OPENED	G258_ROOT "/timestamp-test.txt"
#define G258_UNOPENED	G258_ROOT "/server-made.txt"
#define G258_SERVER	XFS_EXPORT "/g258/server-made.txt"

/* 1960-01-01 01:01:00 UTC, the value upstream's comment names */
#define G258_TS		(-315593940LL)

static void g258_remove_tree(void *unused)
{
	xfs_unlink(G258_UNOPENED);
	xfs_unlink(G258_OPENED);
	xfs_rmdir_settled(G258_ROOT);
}

static int g258_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G258_ROOT), 0);
	return kunit_add_action_or_reset(test, g258_remove_tree, NULL);
}

static void the_client_reports_a_pre_epoch_timestamp(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_EQ(test, g258_setup(test), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G258_OPENED, "", 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_utimes(G258_OPENED, G258_TS, G258_TS), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G258_OPENED, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.atime.tv_sec, G258_TS,
			    "atime came back as %lld", st.atime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, st.mtime.tv_sec, G258_TS,
			    "mtime came back as %lld", st.mtime.tv_sec);
	KUNIT_EXPECT_LT_MSG(test, st.atime.tv_sec, 0LL,
			    "atime wrapped: %lld", st.atime.tv_sec);
	KUNIT_EXPECT_LT_MSG(test, st.mtime.tv_sec, 0LL,
			    "mtime wrapped: %lld", st.mtime.tv_sec);
}

static void a_pre_epoch_timestamp_survives_the_round_trip(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_EQ(test, g258_setup(test), 0);
	/* created on the server, so the client holds no delegation on it */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G258_SERVER, "", 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_utimes(G258_UNOPENED, G258_TS, G258_TS), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G258_UNOPENED, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.atime.tv_sec, G258_TS,
			    "the client read atime back as %lld",
			    st.atime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, st.mtime.tv_sec, G258_TS,
			    "the client read mtime back as %lld",
			    st.mtime.tv_sec);

	/* upstream's post-remount stat: the server's own value */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G258_SERVER, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.atime.tv_sec, G258_TS,
			    "the server stored atime as %lld",
			    st.atime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, st.mtime.tv_sec, G258_TS,
			    "the server stored mtime as %lld",
			    st.mtime.tv_sec);
}

static int g258_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g258_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g258_cases[] = {
	KUNIT_CASE(the_client_reports_a_pre_epoch_timestamp),
	KUNIT_CASE(a_pre_epoch_timestamp_survives_the_round_trip),
	{}
};

static struct kunit_suite g258_suite = {
	.name		= "xfstests/generic/258",
	.suite_init	= g258_suite_init,
	.suite_exit	= g258_suite_exit,
	.test_cases	= g258_cases,
};

kunit_test_suites(&g258_suite);

MODULE_DESCRIPTION("xfstests generic/258 over a loopback NFS mount");
MODULE_LICENSE("GPL");
