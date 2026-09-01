// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/109 over a loopback NFS mount: renames across directory sizes.
 *
 * Upstream was motivated by an XFS directory-format bug where renames
 * misbehaved at particular directory sizes. Over NFS the interesting
 * surface is the client dcache and the server directory as entry counts
 * cross internal thresholds: for each size, populate a directory, rename
 * every entry, verify old names are gone and new ones resolve, then
 * rename the whole directory and re-verify through the new path.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G109_ROOT	XFS_MNT "/g109"

static const int g109_sizes[] = { 1, 8, 64, 256 };

static void g109_remove_tree(void *unused)
{
	char buf[80];
	int s, i;

	for (s = 0; s < ARRAY_SIZE(g109_sizes); s++) {
		for (i = 0; i < g109_sizes[s]; i++) {
			snprintf(buf, sizeof(buf), G109_ROOT "/d%d/f%d", s, i);
			xfs_unlink(buf);
			snprintf(buf, sizeof(buf), G109_ROOT "/d%d/g%d", s, i);
			xfs_unlink(buf);
			snprintf(buf, sizeof(buf), G109_ROOT "/e%d/g%d", s, i);
			xfs_unlink(buf);
		}
		snprintf(buf, sizeof(buf), G109_ROOT "/d%d", s);
		xfs_rmdir(buf);
		snprintf(buf, sizeof(buf), G109_ROOT "/e%d", s);
		xfs_rmdir(buf);
	}
	xfs_rmdir(G109_ROOT);
}

static void renames_survive_every_directory_size(struct kunit *test)
{
	char a[80], b[80];
	int s, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G109_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g109_remove_tree, NULL),
			0);

	for (s = 0; s < ARRAY_SIZE(g109_sizes); s++) {
		int k = g109_sizes[s];

		snprintf(a, sizeof(a), G109_ROOT "/d%d", s);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(a), 0);

		for (i = 0; i < k; i++) {
			snprintf(a, sizeof(a), G109_ROOT "/d%d/f%d", s, i);
			KUNIT_ASSERT_EQ(test,
					xfs_write_new_file(a, "x", 1), 0);
		}
		for (i = 0; i < k; i++) {
			snprintf(a, sizeof(a), G109_ROOT "/d%d/f%d", s, i);
			snprintf(b, sizeof(b), G109_ROOT "/d%d/g%d", s, i);
			KUNIT_ASSERT_EQ_MSG(test, xfs_rename(a, b), 0,
					    "size %d: renaming entry %d", k, i);
		}
		for (i = 0; i < k; i++) {
			snprintf(a, sizeof(a), G109_ROOT "/d%d/f%d", s, i);
			snprintf(b, sizeof(b), G109_ROOT "/d%d/g%d", s, i);
			KUNIT_ASSERT_FALSE_MSG(test, xfs_exists(a),
					       "size %d: old name %d survives",
					       k, i);
			KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(b),
					      "size %d: new name %d missing",
					      k, i);
		}

		/* rename the populated directory itself */
		snprintf(a, sizeof(a), G109_ROOT "/d%d", s);
		snprintf(b, sizeof(b), G109_ROOT "/e%d", s);
		KUNIT_ASSERT_EQ(test, xfs_rename(a, b), 0);
		snprintf(a, sizeof(a), G109_ROOT "/e%d/g%d", s, k - 1);
		KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(a),
				      "size %d: entries unreachable after dir rename",
				      k);
	}
}

static int g109_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g109_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g109_cases[] = {
	KUNIT_CASE_SLOW(renames_survive_every_directory_size),
	{}
};

static struct kunit_suite g109_suite = {
	.name		= "xfstests/generic/109",
	.suite_init	= g109_suite_init,
	.suite_exit	= g109_suite_exit,
	.test_cases	= g109_cases,
};

kunit_test_suites(&g109_suite);

MODULE_DESCRIPTION("xfstests generic/109 over a loopback NFS mount");
MODULE_LICENSE("GPL");
