// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/040 over a loopback NFS mount: link count across many
 * hard links, minus the crash.
 *
 * Third in the dm-flakey family, after generic/034 and generic/039, and the
 * same caveat applies: **this port does not test what generic/040 tests.**
 * Upstream builds a file with 3001 hard links, adds one more, fsyncs, then
 * drops the writes with dm-flakey and remounts. The btrfs bug was that log
 * replay restored a link count smaller than the true one, leaving dangling
 * directory entries that returned ESTALE on access. Without a block device
 * there is no crash and no replay, so the bug itself is out of reach.
 *
 * What ports is the link-count bookkeeping, and over NFS that is a protocol
 * question rather than an on-disk one. nlink is carried in the GETATTR
 * attribute set and cached on the client inode; every LINK and REMOVE has to
 * move it. Upstream's own output is exactly two link counts and the file's
 * contents, which is precisely the part that survives translation:
 *
 *     Link count before rm foo_link_*: 3002
 *     Link count after rm foo_link_*: 1
 *     hello world
 *
 * Both counts are read with a forced revalidation (xfs_kstat uses
 * AT_STATX_FORCE_SYNC), so a client that let its cached nlink drift from the
 * server's answer fails here. generic/039 covers the same attribute for a
 * single link; this covers it at scale and across a bulk unlink.
 *
 * Deviation: upstream's 3001 links exist to push btrfs past the threshold
 * where it starts using extended references, an on-disk format detail with
 * no NFS analogue -- so the exact count is not load-bearing here and is
 * scaled to G040_LINKS to keep the run to a sensible number of round trips.
 * Each link is a real LINK RPC and each removal a real REMOVE.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G040_ROOT	XFS_MNT "/g040"
#define G040_FOO	G040_ROOT "/foo"
#define G040_SERVER	XFS_EXPORT "/g040/foo"

#define G040_TEXT	"hello world\n"
#define G040_LEN	(sizeof(G040_TEXT) - 1)

/* upstream's 3000, scaled: the extref threshold it targets is btrfs-only */
#define G040_LINKS	400

static void g040_linkname(char *buf, size_t sz, int i)
{
	snprintf(buf, sz, G040_ROOT "/foo_link_%04d", i);
}

static void g040_remove_tree(void *unused)
{
	char name[80];
	int i;

	for (i = 1; i <= G040_LINKS + 1; i++) {
		g040_linkname(name, sizeof(name), i);
		xfs_unlink(name);
	}
	xfs_unlink(G040_FOO);
	xfs_rmdir_settled(G040_ROOT);
}

static unsigned int g040_nlink(struct kunit *test, const char *when)
{
	struct kstat st;

	KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(G040_FOO, &st), 0, "%s: stat foo",
			    when);
	return st.nlink;
}

static void link_count_is_correct_after_bulk_link_and_unlink(struct kunit *test)
{
	char name[80];
	char got[G040_LEN];
	unsigned int n;
	ssize_t r;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G040_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g040_remove_tree,
						  NULL), 0);

	/* echo "hello world" > foo */
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G040_FOO, G040_TEXT, G040_LEN), 0);

	/* ln foo foo_link_NNNN, G040_LINKS times */
	for (i = 1; i <= G040_LINKS; i++) {
		g040_linkname(name, sizeof(name), i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_link(G040_FOO, name), 0,
				    "creating link %d of %d", i, G040_LINKS);
	}

	/* upstream's sync before the final link */
	KUNIT_ASSERT_EQ(test, xfs_fsync_path(G040_FOO), 0);

	/* the one extra link, then fsync the inode */
	g040_linkname(name, sizeof(name), G040_LINKS + 1);
	KUNIT_ASSERT_EQ(test, xfs_link(G040_FOO, name), 0);
	KUNIT_EXPECT_EQ_MSG(test, xfs_fsync_path(G040_FOO), 0,
			    "fsync after the final link");

	/* "Link count before rm foo_link_*": the name plus every link */
	n = g040_nlink(test, "before rm");
	KUNIT_EXPECT_EQ_MSG(test, n, (unsigned int)(G040_LINKS + 2),
			    "link count before rm foo_link_* is %u, expected %d",
			    n, G040_LINKS + 2);

	/* rm foo_link_* */
	for (i = 1; i <= G040_LINKS + 1; i++) {
		g040_linkname(name, sizeof(name), i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(name), 0,
				    "removing link %d", i);
	}

	/* "Link count after rm foo_link_*": just foo itself */
	n = g040_nlink(test, "after rm");
	KUNIT_EXPECT_EQ_MSG(test, n, 1U,
			    "link count after rm foo_link_* is %u, expected 1",
			    n);

	/*
	 * "cat $SCRATCH_MNT/foo" -- upstream's check that the inode is still
	 * reachable rather than ESTALE, and that its data survived.
	 */
	r = xfs_read_range(G040_FOO, got, G040_LEN, 0);
	KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)G040_LEN,
			    "read of foo returned %zd", r);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, G040_TEXT, G040_LEN), 0,
			    "foo's content changed");

	/* and the server agrees the data is there */
	r = xfs_read_range(G040_SERVER, got, G040_LEN, 0);
	KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)G040_LEN,
			    "server read of foo returned %zd", r);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, G040_TEXT, G040_LEN), 0,
			    "foo's content differs on the server");
}

static int g040_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g040_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g040_cases[] = {
	KUNIT_CASE_SLOW(link_count_is_correct_after_bulk_link_and_unlink),
	{}
};

static struct kunit_suite g040_suite = {
	.name		= "xfstests/generic/040",
	.suite_init	= g040_suite_init,
	.suite_exit	= g040_suite_exit,
	.test_cases	= g040_cases,
};

kunit_test_suites(&g040_suite);

MODULE_DESCRIPTION("xfstests generic/040 over a loopback NFS mount");
MODULE_LICENSE("GPL");
