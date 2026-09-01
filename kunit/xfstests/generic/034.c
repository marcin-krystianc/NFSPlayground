// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/034 over a loopback NFS mount: directory fsync, minus the
 * crash.
 *
 * **Read this before trusting the port.** Upstream 034 is a crash-consistency
 * test. It builds a directory whose entry for "foo" exists both in the
 * persisted metadata and in the fsync log, drops the writes with dm-flakey,
 * remounts, and checks that log replay did not corrupt the directory's
 * i_size -- the btrfs bug being that a corrupted i_size made rmdir fail with
 * ENOTEMPTY on an empty directory.
 *
 * The crash is the test, and it is not reachable here. dm-flakey needs a
 * block device; the fixture exports tmpfs, and both client and server live in
 * one kernel, so there is no way to drop writes and replay. **This port does
 * not test what generic/034 tests.** Nothing in the ported set does, and the
 * docs say so.
 *
 * What is left is still worth running over NFS, for a reason specific to this
 * filesystem rather than to btrfs. The sequence ends by unlinking every entry
 * and calling rmdir, and over NFS that is exactly where sillyrename bites: if
 * an unlinked file still has a struct file awaiting its delayed fput, the
 * client renames it to .nfsXXXX instead of removing it, the directory is not
 * empty, and rmdir returns ENOTEMPTY -- the same errno as the btrfs bug, from
 * an unrelated cause. Several ports in this set hit that during development,
 * which is why the fixture has xfs_rmdir_settled().
 *
 * So this case deliberately uses the PLAIN rmdir, not the settled one --
 * after an explicit flush_delayed_fput(), which stands in for the task_work
 * that close(2) would have run in userspace but a kernel thread never does.
 * Past that point a surviving .nfsXXXX would be a real client bug, and
 * xfs_rmdir_settled()'s retry loop would hide it. The first version of this
 * port omitted the flush and failed with ENOTEMPTY on both rmdirs; that was
 * the harness, not the client.
 *
 * It also covers something no other port does: fsync on a directory
 * (nfs_fsync_dir), which upstream calls to force the log entry.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G034_ROOT	XFS_MNT "/g034"
#define G034_DIR	G034_ROOT "/test_dir"
#define G034_FOO	G034_DIR "/foo"
#define G034_BAR	G034_DIR "/bar"

static void g034_remove_tree(void *unused)
{
	xfs_unlink(G034_FOO);
	xfs_unlink(G034_BAR);
	xfs_rmdir_settled(G034_DIR);
	xfs_rmdir(G034_ROOT);
}

static void emptied_directory_can_be_removed(struct kunit *test)
{
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G034_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g034_remove_tree,
						  NULL), 0);

	KUNIT_ASSERT_EQ(test, xfs_mkdir(G034_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G034_FOO, "", 0), 0);

	/* upstream's sync: get foo durable before bar is created */
	KUNIT_ASSERT_EQ_MSG(test, xfs_fsync_path(G034_DIR), 0,
			    "fsync of the directory after creating foo");

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G034_BAR, "", 0), 0);

	/*
	 * fsync the directory and then the new file. Directory fsync is
	 * nfs_fsync_dir(); no other port in this set calls it.
	 */
	err = xfs_fsync_path(G034_DIR);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "fsync of the directory: %d", err);
	err = xfs_fsync_path(G034_BAR);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "fsync of bar: %d", err);

	/* upstream's post-remount existence checks */
	KUNIT_EXPECT_TRUE_MSG(test, xfs_exists(G034_FOO), "file foo is missing");
	KUNIT_EXPECT_TRUE_MSG(test, xfs_exists(G034_BAR), "file bar is missing");

	KUNIT_ASSERT_EQ(test, xfs_unlink(G034_FOO), 0);
	KUNIT_ASSERT_EQ(test, xfs_unlink(G034_BAR), 0);

	/*
	 * Flush pending fputs before the rmdir. This is a harness artifact,
	 * not NFS semantics, and the distinction matters for what this
	 * assertion means. In userspace, close(2) completes its fput via
	 * task_work before the syscall returns, so by the time rm exits the
	 * file is truly gone. A KUnit case runs in a kernel thread, which
	 * never runs task_work, so every fput here is deferred -- the file
	 * still holds a reference at unlink time, the client sillyrenames it
	 * to .nfsXXXX, and the directory is not empty. Without this flush the
	 * plain rmdir below cannot succeed for any file that was ever opened,
	 * which says nothing about the client.
	 *
	 * With the flush, the plain rmdir is still the right assertion: it is
	 * the point where userspace's rm would have finished, so a
	 * .nfsXXXX surviving past here would be a real client bug. This is
	 * deliberately not xfs_rmdir_settled(), whose retry loop would hide
	 * exactly that.
	 */
	flush_delayed_fput();

	err = xfs_rmdir(G034_DIR);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "rmdir of an emptied directory returned %d (ENOTEMPTY here means a .nfsXXXX sillyrename entry survived the unlinks)",
			    err);
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G034_DIR),
			       "rmdir didn't succeed");
}

static int g034_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g034_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g034_cases[] = {
	KUNIT_CASE(emptied_directory_can_be_removed),
	{}
};

static struct kunit_suite g034_suite = {
	.name		= "xfstests/generic/034",
	.suite_init	= g034_suite_init,
	.suite_exit	= g034_suite_exit,
	.test_cases	= g034_cases,
};

kunit_test_suites(&g034_suite);

MODULE_DESCRIPTION("xfstests generic/034 over a loopback NFS mount");
MODULE_LICENSE("GPL");
