// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/087 over a loopback NFS mount: truncate permissions.
 *
 * Upstream's fs_perms "t" rows: whether an identity may truncate a file
 * is a write-permission question, answered over NFS by SETATTR(size)
 * against the server's checks. Deviation: upstream also has a "T"
 * variant (fs_perms' second truncate mode); only plain truncate(2)
 * semantics are ported.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G087_ROOT	XFS_MNT "/g087"

static void g087_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G087_FILE	G087_ROOT "/t"

static const struct g087_row {
	umode_t mode;
	uid_t uid;	gid_t gid;
	bool allowed;
} g087_rows[] = {
	{ 0600,  99,  99, true  },	/* owner with w */
	{ 0400,  99,  99, false },	/* owner without w */
	{ 0600, 100,  99, false },	/* other, no perms */
	{ 0660, 100,  99, true  },	/* group w */
	{ 0606, 100, 500, true  },	/* other w */
	{ 0604, 100, 500, false },	/* other r only */
};

static void g087_remove_tree(void *unused)
{
	xfs_unlink(G087_FILE);
	xfs_rmdir(G087_ROOT);
}

static void truncate_follows_write_permission(struct kunit *test)
{
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G087_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G087_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_creds_action, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(g087_rows); i++) {
		const struct g087_row *row = &g087_rows[i];

		xfs_unlink(G087_FILE);
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(G087_FILE, "tttt", 4), 0);
		KUNIT_ASSERT_EQ(test, xfs_chown(G087_FILE, 99, 99), 0);
		KUNIT_ASSERT_EQ(test, xfs_chmod(G087_FILE, row->mode), 0);

		KUNIT_ASSERT_EQ(test,
				xfs_switch_creds(row->uid, row->gid), 0);
		err = xfs_truncate(G087_FILE, 1);
		xfs_restore_creds();

		if (row->allowed)
			KUNIT_EXPECT_EQ_MSG(test, err, 0,
					    "row %d: truncate denied (%d)", i,
					    err);
		else
			KUNIT_EXPECT_EQ_MSG(test, err, -EACCES,
					    "row %d: expected EACCES, got %d",
					    i, err);
	}
}

static int g087_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g087_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g087_cases[] = {
	KUNIT_CASE(truncate_follows_write_permission),
	{}
};

static struct kunit_suite g087_suite = {
	.name		= "xfstests/generic/087",
	.suite_init	= g087_suite_init,
	.suite_exit	= g087_suite_exit,
	.test_cases	= g087_cases,
};

kunit_test_suites(&g087_suite);

MODULE_DESCRIPTION("xfstests generic/087 over a loopback NFS mount");
MODULE_LICENSE("GPL");
