// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/258 over a loopback NFS mount: timestamps before the epoch.
 *
 * Upstream sets one specific timestamp and checks it twice:
 *
 *	touch -t 196001010101 $TESTFILE	# -315593940, Jan 1 1960 01:01
 *	stat -c %X  -> must stay negative
 *	_test_cycle_mount
 *	stat -c %X  -> must still be negative
 *
 * The regression it comes from was ext2/3/4 sign-extending the on-disk value,
 * so the second check -- after the cache is gone -- is the one that mattered.
 * NFSv4 carries times as a signed 64-bit seconds count, so the value has to
 * survive SETATTR, the server's own storage, and GETATTR.
 *
 * The shared fixture cannot cycle the client mount, so the cold check reads
 * the server's copy under the tmpfs export, as generic/029 and generic/169
 * do. Both times are checked: `touch` sets atime and mtime together, and
 * upstream's `%X` is atime.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G258_ROOT	XFS_MNT "/g258"
#define G258_FILE	G258_ROOT "/timestamp-test.txt"
#define G258_SRV_FILE	XFS_EXPORT "/g258/timestamp-test.txt"

/* Jan 1 1960 01:01 UTC, the value upstream's own comment names */
#define G258_TS		(-315593940LL)

static void g258_remove_tree(void *unused)
{
	xfs_unlink(G258_FILE);
	xfs_rmdir(G258_ROOT);
}

static void g258_expect_pre_epoch(struct kunit *test, const char *path,
				  const char *what)
{
	struct kstat st;

	KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0, "%s: stat", what);

	/* upstream's check: the value must not have wrapped */
	KUNIT_EXPECT_LT_MSG(test, st.atime.tv_sec, (time64_t)0,
			    "%s: timestamp wrapped: %lld", what,
			    (long long)st.atime.tv_sec);
	/* and it must be the value that was set, not merely some negative one */
	KUNIT_EXPECT_EQ_MSG(test, st.atime.tv_sec, (time64_t)G258_TS,
			    "%s: atime came back as %lld, expected %lld", what,
			    (long long)st.atime.tv_sec, G258_TS);
	KUNIT_EXPECT_EQ_MSG(test, st.mtime.tv_sec, (time64_t)G258_TS,
			    "%s: mtime came back as %lld, expected %lld", what,
			    (long long)st.mtime.tv_sec, G258_TS);
}

static void pre_epoch_timestamps_round_trip(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G258_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g258_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G258_FILE, "e", 1), 0);

	KUNIT_ASSERT_EQ(test, xfs_utimes(G258_FILE, G258_TS, G258_TS), 0);

	/* live, through the client */
	g258_expect_pre_epoch(test, G258_FILE, "client");
	/* and cold, from the server's own copy */
	g258_expect_pre_epoch(test, G258_SRV_FILE, "server");
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
	KUNIT_CASE(pre_epoch_timestamps_round_trip),
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
