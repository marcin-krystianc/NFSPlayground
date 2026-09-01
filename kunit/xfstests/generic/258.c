// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/258 over a loopback NFS mount: timestamps before the epoch.
 *
 * Upstream regression: 64-bit sign handling of pre-1970 timestamps
 * (ext* once sign-extended wrongly). NFSv4 carries times as signed
 * seconds, so negative timestamps must round-trip through SETATTR and
 * GETATTR exactly.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G258_ROOT	XFS_MNT "/g258"

static void g258_remove_tree(void *unused)
{
	xfs_unlink(G258_ROOT "/f");
	xfs_rmdir(G258_ROOT);
}

static void pre_epoch_timestamps_round_trip(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G258_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g258_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G258_ROOT "/f", "e", 1), 0);

	/* one hour and one day before the epoch, as upstream */
	KUNIT_ASSERT_EQ(test, xfs_utimes(G258_ROOT "/f", -3600, -86400), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G258_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.atime.tv_sec, (time64_t)-3600,
			    "pre-epoch atime came back as %lld",
			    (long long)st.atime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, st.mtime.tv_sec, (time64_t)-86400,
			    "pre-epoch mtime came back as %lld",
			    (long long)st.mtime.tv_sec);
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
