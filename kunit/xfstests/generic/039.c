// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/039 over a loopback NFS mount: dropping one hard link,
 * minus the crash.
 *
 * The same caveat as generic/034, and for the same reason. Upstream 039 is a
 * crash-consistency test: create a file with two hard links, sync, remove one
 * link, fsync the inode, then drop the writes with dm-flakey and remount. The
 * btrfs bug was that log replay left the parent directory with a wrong i_size
 * and dangling index entries, so rmdir failed with ENOTEMPTY forever after.
 *
 * dm-flakey needs a block device, the fixture exports tmpfs, and client and
 * server share one kernel, so the crash and replay cannot happen. **This port
 * does not test what generic/039 tests.**
 *
 * The residue is a genuine NFS check, and a different one from 034's. Link
 * count is a protocol-visible attribute: the client learns nlink from GETATTR,
 * caches it on the inode, and REMOVE of one of two hard links must be
 * reflected in the surviving link's attributes. So this asserts nlink at each
 * step -- 1 after creation, 2 after the link, 1 again after removing one --
 * with a forced revalidation each time (xfs_kstat uses AT_STATX_FORCE_SYNC),
 * plus that the surviving name still reads its data. A stale cached nlink
 * would pass a test that only checked the file still exists.
 *
 * The teardown is upstream's and is checked rather than best-effort: after
 * the last link goes, both directories must rmdir cleanly. As in 034 this
 * uses the plain rmdir rather than xfs_rmdir_settled(), preceded by an
 * explicit flush_delayed_fput() -- see 034's header for why that flush is a
 * harness artifact rather than a concession. Both rmdirs failed with
 * ENOTEMPTY before the flush was added.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G039_ROOT	XFS_MNT "/g039"
#define G039_A		G039_ROOT "/a"
#define G039_B		G039_A "/b"
#define G039_FOO	G039_B "/foo"
#define G039_BAR	G039_B "/bar"

#define G039_TEXT	"hello world\n"
#define G039_LEN	(sizeof(G039_TEXT) - 1)

static void g039_remove_tree(void *unused)
{
	xfs_unlink(G039_FOO);
	xfs_unlink(G039_BAR);
	xfs_rmdir_settled(G039_B);
	xfs_rmdir_settled(G039_A);
	xfs_rmdir(G039_ROOT);
}

static void g039_expect_nlink(struct kunit *test, const char *path,
			      unsigned int want, const char *when)
{
	struct kstat st;

	KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0, "%s: stat %s", when,
			    path);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, want,
			    "%s: %s has nlink %u, expected %u", when, path,
			    st.nlink, want);
}

static void removing_one_hard_link_leaves_the_other_intact(struct kunit *test)
{
	char got[G039_LEN];
	ssize_t n;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G039_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g039_remove_tree,
						  NULL), 0);

	/* mkdir -p a/b; echo > a/b/foo; ln a/b/foo a/b/bar */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G039_A), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G039_B), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G039_FOO, G039_TEXT, G039_LEN), 0);
	g039_expect_nlink(test, G039_FOO, 1, "after creating foo");

	KUNIT_ASSERT_EQ_MSG(test, xfs_link(G039_FOO, G039_BAR), 0,
			    "creating the second hard link");
	g039_expect_nlink(test, G039_FOO, 2, "after linking bar");
	g039_expect_nlink(test, G039_BAR, 2, "after linking bar");

	/* upstream's sync */
	KUNIT_ASSERT_EQ(test, xfs_fsync_path(G039_FOO), 0);

	/* remove one link and fsync the inode through the surviving name */
	KUNIT_ASSERT_EQ(test, xfs_unlink(G039_BAR), 0);
	err = xfs_fsync_path(G039_FOO);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "fsync of foo after unlinking bar: %d",
			    err);

	/*
	 * The check the crash version cannot reach here, but which is
	 * protocol-visible on its own: the surviving link's cached nlink must
	 * have followed the REMOVE back down to 1.
	 */
	g039_expect_nlink(test, G039_FOO, 1, "after unlinking bar");
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G039_BAR),
			       "bar still exists after unlink");

	/* and the data is still reachable through the name that remains */
	n = xfs_read_range(G039_FOO, got, G039_LEN, 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G039_LEN,
			    "read of foo returned %zd", n);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, G039_TEXT, G039_LEN), 0,
			    "foo's content changed when bar was unlinked");

	/* upstream's teardown, asserted rather than assumed */
	KUNIT_ASSERT_EQ(test, xfs_unlink(G039_FOO), 0);
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

	err = xfs_rmdir(G039_B);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "rmdir of a/b returned %d (ENOTEMPTY here means a .nfsXXXX sillyrename entry survived)",
			    err);
	err = xfs_rmdir(G039_A);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "rmdir of a returned %d", err);
}

static int g039_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g039_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g039_cases[] = {
	KUNIT_CASE(removing_one_hard_link_leaves_the_other_intact),
	{}
};

static struct kunit_suite g039_suite = {
	.name		= "xfstests/generic/039",
	.suite_init	= g039_suite_init,
	.suite_exit	= g039_suite_exit,
	.test_cases	= g039_cases,
};

kunit_test_suites(&g039_suite);

MODULE_DESCRIPTION("xfstests generic/039 over a loopback NFS mount");
MODULE_LICENSE("GPL");
