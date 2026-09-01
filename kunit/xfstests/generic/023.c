// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/023 over a loopback NFS mount: rename semantics matrix.
 *
 * Upstream drives src/renameat2 without flags through the POSIX rename
 * corner cases. Every row here is a RENAME RPC and a distinct errno the
 * client must carry back faithfully: missing source, file over file, file
 * over directory, directory over file, directory over non-empty
 * directory, directory into its own subtree, and the same-inode
 * hardlink no-op the VFS implements for POSIX.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G023_ROOT	XFS_MNT "/g023"

static void g023_remove_tree(void *unused)
{
	xfs_unlink(G023_ROOT "/f1");
	xfs_unlink(G023_ROOT "/f2");
	xfs_unlink(G023_ROOT "/h1");
	xfs_unlink(G023_ROOT "/h2");
	xfs_unlink(G023_ROOT "/d2/x");
	xfs_rmdir(G023_ROOT "/d1/sub");
	xfs_rmdir(G023_ROOT "/d1");
	xfs_rmdir(G023_ROOT "/d2");
	xfs_rmdir(G023_ROOT "/d3");
	xfs_rmdir(G023_ROOT);
}

static void rename_covers_the_posix_errno_matrix(struct kunit *test)
{
	char c;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g023_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G023_ROOT "/f1", "1", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G023_ROOT "/f2", "2", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d1"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d1/sub"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d2"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G023_ROOT "/d2/x", "x", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d3"), 0);

	/* missing source */
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/nope", G023_ROOT "/f1"), -ENOENT);

	/* file over directory / directory over file */
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/f1", G023_ROOT "/d3"), -EISDIR);
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/d3", G023_ROOT "/f1"), -ENOTDIR);

	/* directory over non-empty directory */
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/d1", G023_ROOT "/d2"), -ENOTEMPTY);

	/* directory into its own subtree */
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/d1", G023_ROOT "/d1/sub/d1"),
			-EINVAL);

	/* directory over empty directory works */
	KUNIT_EXPECT_EQ(test, xfs_rename(G023_ROOT "/d1", G023_ROOT "/d3"), 0);
	KUNIT_EXPECT_TRUE(test, xfs_exists(G023_ROOT "/d3/sub"));
	KUNIT_ASSERT_EQ(test, xfs_rename(G023_ROOT "/d3", G023_ROOT "/d1"), 0);

	/* file over file replaces, and the content is the source's */
	KUNIT_EXPECT_EQ(test, xfs_rename(G023_ROOT "/f1", G023_ROOT "/f2"), 0);
	KUNIT_EXPECT_FALSE(test, xfs_exists(G023_ROOT "/f1"));
	KUNIT_ASSERT_EQ(test, xfs_read_range(G023_ROOT "/f2", &c, 1, 0),
			(ssize_t)1);
	KUNIT_EXPECT_EQ(test, c, (char)0x31);

	/* same-inode hardlinks: rename is a POSIX no-op, both names stay */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G023_ROOT "/h1", "h", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_link(G023_ROOT "/h1", G023_ROOT "/h2"), 0);
	KUNIT_EXPECT_EQ(test, xfs_rename(G023_ROOT "/h1", G023_ROOT "/h2"), 0);
	KUNIT_EXPECT_TRUE_MSG(test,
			      xfs_exists(G023_ROOT "/h1") &&
			      xfs_exists(G023_ROOT "/h2"),
			      "the same-inode rename no-op removed a name");
}

static int g023_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g023_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g023_cases[] = {
	KUNIT_CASE(rename_covers_the_posix_errno_matrix),
	{}
};

static struct kunit_suite g023_suite = {
	.name		= "xfstests/generic/023",
	.suite_init	= g023_suite_init,
	.suite_exit	= g023_suite_exit,
	.test_cases	= g023_cases,
};

kunit_test_suites(&g023_suite);

MODULE_DESCRIPTION("xfstests generic/023 over a loopback NFS mount");
MODULE_LICENSE("GPL");
