// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/123 over a loopback NFS mount: unprivileged writes into a root directory.
 *
 * Upstream: a user must not be able to modify files in a 755
 * root-owned directory -- append, overwrite, remove and rename must all
 * fail, and the file's content must be untouched afterwards. Each denial
 * is the server refusing the corresponding RPC.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G123_ROOT	XFS_MNT "/g123"

static void g123_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G123_DIR	G123_ROOT "/123subdir"
#define G123_FILE	G123_DIR "/data_coherency.txt"

static void g123_remove_tree(void *unused)
{
	xfs_unlink(G123_FILE);
	xfs_rmdir(G123_DIR);
	xfs_rmdir(G123_ROOT);
}

static void other_users_cannot_touch_roots_files(struct kunit *test)
{
	struct file *f;
	char buf[16];
	ssize_t n;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G123_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G123_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G123_DIR, 0755), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g123_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g123_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G123_FILE, "foo\nbar\n", 8),
			0);

	KUNIT_ASSERT_EQ(test, xfs_switch_creds(99, 99), 0);

	/* append and overwrite are opens for write: EACCES */
	f = filp_open(G123_FILE, O_WRONLY | O_APPEND, 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(f));
	f = filp_open(G123_FILE, O_WRONLY | O_TRUNC, 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(f));
	/* creating a sibling in a 755 root directory: EACCES */
	f = filp_open(G123_DIR "/mine", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_EXPECT_TRUE(test, IS_ERR(f));
	/* remove and rename: EACCES */
	err = xfs_unlink(G123_FILE);
	KUNIT_EXPECT_EQ_MSG(test, err, -EACCES, "unlink as uid 99: %d", err);
	err = xfs_rename(G123_FILE, G123_DIR "/data_coherency2.txt");
	KUNIT_EXPECT_EQ_MSG(test, err, -EACCES, "rename as uid 99: %d", err);

	xfs_restore_creds();

	/* nothing changed */
	n = xfs_read_range(G123_FILE, buf, 8, 0);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)8);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, "foo\nbar\n", 8), 0,
			    "the denied operations changed the file");
}

static int g123_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g123_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g123_cases[] = {
	KUNIT_CASE(other_users_cannot_touch_roots_files),
	{}
};

static struct kunit_suite g123_suite = {
	.name		= "xfstests/generic/123",
	.suite_init	= g123_suite_init,
	.suite_exit	= g123_suite_exit,
	.test_cases	= g123_cases,
};

kunit_test_suites(&g123_suite);

MODULE_DESCRIPTION("xfstests generic/123 over a loopback NFS mount");
MODULE_LICENSE("GPL");
