// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/035 over a loopback NFS mount: rename over an open target.
 *
 * Upstream's t_rename_overwrite opens the target, renames another name over
 * it, then fstat()s the still-open file and requires nlink == 0. It is run
 * twice: once on a pair of regular files, once on a pair of directories.
 *
 * xfstests keeps a separate golden image for NFS (tests/generic/035.out.nfs)
 * because both halves differ here, and it says what to expect:
 *
 *	overwriting regular file:
 *	nlink is 1, should be 0
 *	overwriting directory:
 *	t_rename_overwrite: fstat(3): Stale file handle
 *
 * The file case keeps a link because the client sillyrenames the open target
 * to a .nfsXXXX name, so nlink stays 1 until the last close and only then
 * does the deferred REMOVE drop it. The directory case cannot be
 * sillyrenamed, so the rename really removes the target server-side and the
 * filehandle held open goes stale -- which is why upstream's fstat fails
 * rather than reporting a count.
 *
 * Both are pinned below: the file case end to end (nlink 1, a .nfs entry in
 * the directory, and one name left after close and settling), and the
 * directory case as ESTALE from the held directory.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G035_ROOT	XFS_MNT "/g035"

#include <linux/mount.h>

struct g035_diriter {
	struct dir_context ctx;
	int entries;		/* excluding . and .. */
	bool saw_silly;
};

static bool g035_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g035_diriter *it = container_of(ctx, struct g035_diriter, ctx);

	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	it->entries++;
	if (len >= 4 && !memcmp(name, ".nfs", 4))
		it->saw_silly = true;
	return true;
}

static int g035_scan_dir(struct kunit *test, int *entries, bool *saw_silly)
{
	struct g035_diriter it = { .ctx.actor = g035_actor };
	struct file *d;
	int err;

	d = filp_open(G035_ROOT, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(d))
		return PTR_ERR(d);
	err = iterate_dir(d, &it.ctx);
	filp_close(d, NULL);
	*entries = it.entries;
	*saw_silly = it.saw_silly;
	return err;
}

static void g035_remove_tree(void *unused)
{
	xfs_unlink(G035_ROOT "/file1");
	xfs_unlink(G035_ROOT "/file2");
	xfs_rmdir(G035_ROOT "/dir1");
	xfs_rmdir(G035_ROOT "/dir2");
	xfs_rmdir_settled(G035_ROOT);
}

static void rename_over_an_open_target_sillyrenames(struct kunit *test)
{
	struct kstat st;
	struct file *held;
	int entries, tries;
	bool saw_silly;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G035_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g035_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G035_ROOT "/file1", "1", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G035_ROOT "/file2", "2", 1), 0);

	held = filp_open(G035_ROOT "/file2", O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(held));

	KUNIT_ASSERT_EQ(test,
			xfs_rename(G035_ROOT "/file1", G035_ROOT "/file2"), 0);

	/*
	 * Upstream (local fs): the open target's nlink is now 0. NFS: the
	 * client sillyrenamed it, so it still has one name -- the .nfs one.
	 */
	KUNIT_ASSERT_EQ(test,
			vfs_getattr(&held->f_path, &st, STATX_BASIC_STATS,
				    AT_STATX_FORCE_SYNC), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, 1U,
			    "expected the sillyrenamed target to keep 1 link, got %u",
			    st.nlink);

	KUNIT_ASSERT_EQ(test, g035_scan_dir(test, &entries, &saw_silly), 0);
	KUNIT_EXPECT_TRUE_MSG(test, saw_silly,
			      "no .nfs sillyrename entry while the target is open");
	KUNIT_EXPECT_EQ_MSG(test, entries, 2,
			    "expected file2 plus one .nfs entry, found %d", entries);

	/* close: the deferred REMOVE reaps the silly name */
	filp_close(held, NULL);
	for (tries = 0; tries < 20; tries++) {
		flush_delayed_fput();
		KUNIT_ASSERT_EQ(test,
				g035_scan_dir(test, &entries, &saw_silly), 0);
		if (!saw_silly)
			break;
		msleep(100);
	}
	KUNIT_EXPECT_FALSE_MSG(test, saw_silly,
			       "the .nfs entry outlived the close");
	KUNIT_EXPECT_EQ(test, entries, 1);
	KUNIT_EXPECT_TRUE(test, xfs_exists(G035_ROOT "/file2"));
	KUNIT_EXPECT_FALSE(test, xfs_exists(G035_ROOT "/file1"));
}

/*
 * "overwriting directory:" -- t_rename_overwrite's second run. A directory
 * cannot be sillyrenamed, so the rename removes the open target for real and
 * the held filehandle is stale: upstream's fstat(3) fails here instead of
 * printing a link count.
 */
static void rename_over_an_open_directory_goes_stale(struct kunit *test)
{
	struct kstat st = {};
	struct file *held;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G035_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g035_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mkdir(G035_ROOT "/dir1"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G035_ROOT "/dir2"), 0);

	held = filp_open(G035_ROOT "/dir2", O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(held));

	KUNIT_ASSERT_EQ(test,
			xfs_rename(G035_ROOT "/dir1", G035_ROOT "/dir2"), 0);

	err = vfs_getattr(&held->f_path, &st, STATX_BASIC_STATS,
			  AT_STATX_FORCE_SYNC);
	filp_close(held, NULL);

	KUNIT_EXPECT_EQ_MSG(test, err, -ESTALE,
			    "expected ESTALE from the overwritten directory, got %d (nlink %u)",
			    err, err ? 0 : st.nlink);

	/* the surviving name is the renamed source, and dir1 is gone */
	KUNIT_EXPECT_TRUE(test, xfs_exists(G035_ROOT "/dir2"));
	KUNIT_EXPECT_FALSE(test, xfs_exists(G035_ROOT "/dir1"));
}

static int g035_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g035_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g035_cases[] = {
	KUNIT_CASE(rename_over_an_open_target_sillyrenames),
	KUNIT_CASE(rename_over_an_open_directory_goes_stale),
	{}
};

static struct kunit_suite g035_suite = {
	.name		= "xfstests/generic/035",
	.suite_init	= g035_suite_init,
	.suite_exit	= g035_suite_exit,
	.test_cases	= g035_cases,
};

kunit_test_suites(&g035_suite);

MODULE_DESCRIPTION("xfstests generic/035 over a loopback NFS mount");
MODULE_LICENSE("GPL");
