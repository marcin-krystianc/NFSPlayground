// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/193 over a loopback NFS mount: setattr permission checks.
 *
 * Upstream: which SETATTRs an unprivileged owner and a non-owner may
 * perform. Ported rows: a non-owner can neither chmod, chown, set
 * explicit times, nor truncate a 644 file; the owner can chmod and set
 * times but cannot give the file away (chown stays privileged).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G193_ROOT	XFS_MNT "/g193"

static void g193_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G193_FILE	G193_ROOT "/f"

static void g193_remove_tree(void *unused)
{
	xfs_unlink(G193_FILE);
	xfs_rmdir(G193_ROOT);
}

static void setattr_rights_split_owner_and_stranger(struct kunit *test)
{
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G193_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G193_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g193_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g193_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G193_FILE, "s", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G193_FILE, 99, 99), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G193_FILE, 0644), 0);

	/* a stranger may do none of it */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(100, 100), 0);
	err = xfs_chmod(G193_FILE, 0666);
	KUNIT_EXPECT_EQ_MSG(test, err, -EPERM, "stranger chmod: %d", err);
	err = xfs_chown(G193_FILE, 100, 100);
	KUNIT_EXPECT_EQ_MSG(test, err, -EPERM, "stranger chown: %d", err);
	err = xfs_utimes(G193_FILE, 1000, 1000);
	KUNIT_EXPECT_EQ_MSG(test, err, -EPERM, "stranger utimes: %d", err);
	err = xfs_truncate(G193_FILE, 0);
	KUNIT_EXPECT_EQ_MSG(test, err, -EACCES, "stranger truncate: %d", err);
	xfs_restore_creds();

	/* the owner can chmod and set explicit times... */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(99, 99), 0);
	err = xfs_chmod(G193_FILE, 0600);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "owner chmod: %d", err);
	err = xfs_utimes(G193_FILE, 2000, 3000);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "owner utimes: %d", err);
	/* ...but cannot give the file away */
	err = xfs_chown(G193_FILE, 100, 100);
	KUNIT_EXPECT_EQ_MSG(test, err, -EPERM,
			    "owner chown-away should be EPERM, got %d", err);
	xfs_restore_creds();

	/* and the explicit times landed */
	{
		struct kstat st;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G193_FILE, &st), 0);
		KUNIT_EXPECT_EQ(test, st.atime.tv_sec, (time64_t)2000);
		KUNIT_EXPECT_EQ(test, st.mtime.tv_sec, (time64_t)3000);
		KUNIT_EXPECT_EQ(test, st.mode & 0777, 0600);
	}
}

static int g193_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g193_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g193_cases[] = {
	KUNIT_CASE(setattr_rights_split_owner_and_stranger),
	{}
};

static struct kunit_suite g193_suite = {
	.name		= "xfstests/generic/193",
	.suite_init	= g193_suite_init,
	.suite_exit	= g193_suite_exit,
	.test_cases	= g193_cases,
};

kunit_test_suites(&g193_suite);

MODULE_DESCRIPTION("xfstests generic/193 over a loopback NFS mount");
MODULE_LICENSE("GPL");
