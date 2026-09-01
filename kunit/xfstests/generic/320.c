// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/320 over a loopback NFS mount: heavy removal workload.
 *
 * Upstream creates a large population of files and removes them all,
 * hunting for removal-path bugs under bulk. Scaled port: 2000 files in
 * two directories, bulk REMOVE storm, then the directories must rmdir
 * cleanly and the space and inode counts must come back.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G320_ROOT	XFS_MNT "/g320"

#define G320_PER_DIR	1000

static void g320_remove_tree(void *unused)
{
	char buf[64];
	int d, i;

	for (d = 0; d < 2; d++) {
		for (i = 0; i < G320_PER_DIR; i++) {
			snprintf(buf, sizeof(buf), G320_ROOT "/d%d/f%04d", d, i);
			xfs_unlink(buf);
		}
		snprintf(buf, sizeof(buf), G320_ROOT "/d%d", d);
		xfs_rmdir(buf);
	}
	xfs_rmdir(G320_ROOT);
}

static void bulk_removal_returns_every_resource(struct kunit *test)
{
	struct kstatfs before, after;
	char name[64];
	int d, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &before), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G320_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g320_remove_tree, NULL),
			0);

	for (d = 0; d < 2; d++) {
		snprintf(name, sizeof(name), G320_ROOT "/d%d", d);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(name), 0);
		for (i = 0; i < G320_PER_DIR; i++) {
			snprintf(name, sizeof(name),
				 G320_ROOT "/d%d/f%04d", d, i);
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_write_new_file(name, "z", 1), 0,
					    "creating %s", name);
		}
	}

	for (d = 0; d < 2; d++) {
		for (i = 0; i < G320_PER_DIR; i++) {
			snprintf(name, sizeof(name),
				 G320_ROOT "/d%d/f%04d", d, i);
			KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(name), 0,
					    "removing %s", name);
		}
		snprintf(name, sizeof(name), G320_ROOT "/d%d", d);
		KUNIT_ASSERT_EQ_MSG(test, xfs_rmdir_settled(name), 0,
				    "%s not empty after bulk removal", name);
	}
	KUNIT_ASSERT_EQ(test, xfs_rmdir(G320_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G320_ROOT), 0);	/* for the action */

	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &after), 0);
	KUNIT_EXPECT_GE_MSG(test, (long long)after.f_ffree + 8,
			    (long long)before.f_ffree,
			    "inodes leaked: ffree %llu -> %llu",
			    (u64)before.f_ffree, (u64)after.f_ffree);
}

static int g320_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g320_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g320_cases[] = {
	KUNIT_CASE_SLOW(bulk_removal_returns_every_resource),
	{}
};

static struct kunit_suite g320_suite = {
	.name		= "xfstests/generic/320",
	.suite_init	= g320_suite_init,
	.suite_exit	= g320_suite_exit,
	.test_cases	= g320_cases,
};

kunit_test_suites(&g320_suite);

MODULE_DESCRIPTION("xfstests generic/320 over a loopback NFS mount");
MODULE_LICENSE("GPL");
