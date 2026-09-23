// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/023 over a loopback NFS mount: the full rename
 * errno/type matrix.
 *
 * Upstream (common/renameat2's _rename_tests) drives src/renameat2
 * without flags through every combination of source and destination type
 * -- none/regular/symlink/directory/non-empty-directory, 5x5 -- once with
 * source and destination in the same directory and once across two
 * directories, 50 rows total, each checked against generic/023.out's
 * exact errno text and each success row's resulting types.
 *
 * An earlier version of this port hand-picked 8 same-directory errno
 * cases and never exercised the cross-directory dimension at all --
 * upstream's actual coverage is the full matrix, including the
 * symlink-source and symlink-destination rows and every directory-type
 * combination, and cross-directory rename is a materially different RPC
 * shape (the parent's change_attr on both ends has to update) that the
 * hand-picked subset never reached.
 *
 * g023_expected() below is the same rule POSIX rename(2)/vfs_rename()
 * implements, not something read off the golden file: a missing source
 * is ENOENT; a non-directory source replacing a directory is EISDIR; a
 * directory source replacing a non-directory is ENOTDIR; a directory
 * source replacing a non-empty directory is ENOTEMPTY; every other
 * combination succeeds and the destination ends up holding whatever the
 * source was.
 *
 * Two cases beyond upstream's matrix are kept from the earlier version,
 * both real POSIX properties this repo found worth pinning: renaming a
 * directory into its own subtree (EINVAL), and the same-inode hardlink
 * no-op (rename between two hardlinks of one inode removes neither name).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G023_ROOT	XFS_MNT "/g023"

enum g023_type {
	G023_NONE,
	G023_REGU,
	G023_SYMB,
	G023_DIRE,
	G023_TREE,
};

static const char *const g023_type_names[] = {
	[G023_NONE] = "none",
	[G023_REGU] = "regu",
	[G023_SYMB] = "symb",
	[G023_DIRE] = "dire",
	[G023_TREE] = "tree",
};

static void g023_bar_path(char *buf, size_t len, const char *dir)
{
	snprintf(buf, len, "%s/bar", dir);
}

/* common/renameat2's _setup_one() */
static void g023_setup_one(struct kunit *test, const char *path,
			   enum g023_type t)
{
	char bar[192];

	switch (t) {
	case G023_NONE:
		return;
	case G023_REGU:
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "foo\n", 4), 0);
		return;
	case G023_SYMB:
		KUNIT_ASSERT_EQ(test, xfs_symlink("foo", path), 0);
		return;
	case G023_DIRE:
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		return;
	case G023_TREE:
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		g023_bar_path(bar, sizeof(bar), path);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(bar, "foo\n", 4), 0);
		return;
	}
}

/* common/renameat2's _showtype_one(): lstat-like, a dangling symlink is itself */
static enum g023_type g023_showtype(const char *path)
{
	struct kstat st;
	char bar[192];

	if (xfs_kstat(path, &st))
		return G023_NONE;
	if (S_ISDIR(st.mode)) {
		g023_bar_path(bar, sizeof(bar), path);
		return xfs_exists(bar) ? G023_TREE : G023_DIRE;
	}
	if (S_ISLNK(st.mode))
		return G023_SYMB;
	return G023_REGU;
}

/*
 * common/renameat2's _cleanup_one(). xfs_write_new_file() (in the TREE
 * case, for "bar") closes from a kernel thread, which defers the final
 * fput to a workqueue; unlinking a file whose struct file is still alive
 * sillyrenames it to a transient .nfsXXXX entry instead of truly removing
 * it, so the rmdir() right after would see a non-empty directory. Settle
 * first, same as generic/637.
 */
static void g023_cleanup_one(struct kunit *test, const char *path)
{
	struct kstat st;
	char bar[192];
	int err;

	if (xfs_kstat(path, &st))
		return;
	if (S_ISDIR(st.mode)) {
		g023_bar_path(bar, sizeof(bar), path);
		xfs_settle_fput();
		err = xfs_unlink(bar);
		KUNIT_EXPECT_TRUE_MSG(test, err == 0 || err == -ENOENT,
				      "cleanup: unlink %s: %d", bar, err);
		err = xfs_rmdir(path);
		KUNIT_EXPECT_EQ_MSG(test, err, 0, "cleanup: rmdir %s: %d",
				    path, err);
	} else {
		err = xfs_unlink(path);
		KUNIT_EXPECT_EQ_MSG(test, err, 0, "cleanup: unlink %s: %d",
				    path, err);
	}
}

/* vfs_rename()'s actual rule, not the golden file's transcription of it */
static int g023_expected(enum g023_type stype, enum g023_type dtype)
{
	bool sdir = stype == G023_DIRE || stype == G023_TREE;
	bool ddir = dtype == G023_DIRE || dtype == G023_TREE;

	if (stype == G023_NONE)
		return -ENOENT;
	if (!sdir)
		return ddir ? -EISDIR : 0;
	/* directory source */
	if (!ddir && dtype != G023_NONE)
		return -ENOTDIR;
	if (dtype == G023_TREE)
		return -ENOTEMPTY;
	return 0;
}

static void g023_run_matrix(struct kunit *test, const char *src,
			    const char *dst)
{
	enum g023_type stype, dtype;

	for (stype = G023_NONE; stype <= G023_TREE; stype++) {
		for (dtype = G023_NONE; dtype <= G023_TREE; dtype++) {
			int expected = g023_expected(stype, dtype);
			int err;

			g023_setup_one(test, src, stype);
			g023_setup_one(test, dst, dtype);

			err = xfs_rename(src, dst);
			KUNIT_EXPECT_EQ_MSG(test, err, expected,
					    "%s/%s: expected %d, got %d",
					    g023_type_names[stype],
					    g023_type_names[dtype],
					    expected, err);

			if (err == 0) {
				KUNIT_EXPECT_EQ_MSG(test,
						    g023_showtype(src),
						    G023_NONE,
						    "%s/%s: source survived a successful rename",
						    g023_type_names[stype],
						    g023_type_names[dtype]);
				KUNIT_EXPECT_EQ_MSG(test,
						    g023_showtype(dst), stype,
						    "%s/%s: destination is %s after rename, not the source's type",
						    g023_type_names[stype],
						    g023_type_names[dtype],
						    g023_type_names[g023_showtype(dst)]);
			}

			g023_cleanup_one(test, src);
			g023_cleanup_one(test, dst);
		}
	}
}

static void g023_remove_tree(void *unused)
{
	xfs_unlink(G023_ROOT "/src");
	xfs_unlink(G023_ROOT "/dst");
	xfs_unlink(G023_ROOT "/x/src");
	xfs_unlink(G023_ROOT "/y/dst");
	xfs_rmdir(G023_ROOT "/x");
	xfs_rmdir(G023_ROOT "/y");
	xfs_unlink(G023_ROOT "/d1/sub");
	xfs_rmdir(G023_ROOT "/d1/sub");
	xfs_rmdir(G023_ROOT "/d1");
	xfs_unlink(G023_ROOT "/h1");
	xfs_unlink(G023_ROOT "/h2");
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

	/* upstream: same-directory renames, the full 5x5 matrix */
	g023_run_matrix(test, G023_ROOT "/src", G023_ROOT "/dst");

	/* upstream: cross-directory renames, the same 5x5 matrix */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/x"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/y"), 0);
	g023_run_matrix(test, G023_ROOT "/x/src", G023_ROOT "/y/dst");

	/* not in upstream: directory into its own subtree */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d1"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G023_ROOT "/d1/sub"), 0);
	KUNIT_EXPECT_EQ(test,
			xfs_rename(G023_ROOT "/d1", G023_ROOT "/d1/sub/d1"),
			-EINVAL);
	KUNIT_ASSERT_EQ(test, xfs_rmdir(G023_ROOT "/d1/sub"), 0);
	KUNIT_ASSERT_EQ(test, xfs_rmdir(G023_ROOT "/d1"), 0);

	/* not in upstream: same-inode hardlinks, rename is a POSIX no-op */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G023_ROOT "/h1", "h", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_link(G023_ROOT "/h1", G023_ROOT "/h2"), 0);
	KUNIT_EXPECT_EQ(test, xfs_rename(G023_ROOT "/h1", G023_ROOT "/h2"), 0);
	KUNIT_EXPECT_TRUE_MSG(test,
			      xfs_exists(G023_ROOT "/h1") &&
			      xfs_exists(G023_ROOT "/h2"),
			      "the same-inode rename no-op removed a name");
	KUNIT_ASSERT_EQ(test, xfs_read_range(G023_ROOT "/h1", &c, 1, 0),
			(ssize_t)1);
	KUNIT_EXPECT_EQ(test, c, (char)'h');
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
	KUNIT_CASE_SLOW(rename_covers_the_posix_errno_matrix),
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
