// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/041 over a loopback NFS mount: removing and recreating a
 * hard link under the same name, minus the crash.
 *
 * Fourth of the dm-flakey family (034, 039, 040), and the same caveat:
 * **this port does not test what generic/041 tests.** Upstream's bug made
 * btrfs' fsync log replay fail with EOVERFLOW and left the filesystem
 * unmountable. Without a block device there is no replay.
 *
 * Its sequence is worth porting anyway, because it is the one thing in this
 * family that is not just a link count. Upstream removes a link, adds new
 * ones, and then **recreates a link under the name it just removed** before
 * fsyncing. Over NFS that is a dentry-cache question: the client holds a
 * negative or stale dentry for foo_link_0001 after the REMOVE, and the
 * subsequent LINK has to make the name resolve to the inode again. A client
 * that cached the removal too aggressively would fail to see the recreated
 * name, and a client that cached the old dentry would resolve it without
 * asking. Neither shows up in the link count alone, which is why this port
 * checks name-by-name existence the way upstream does.
 *
 * The arithmetic is upstream's, generalised over G041_LINKS = N:
 *
 *   start                       nlink = N + 1   (foo plus N links)
 *   rm link_0001                        N
 *   ln link_{N+1}                       N + 1
 *   ln link_0001   (name reused)        N + 2
 *   rm link_0002                        N + 1
 *   ln link_{N+2}                       N + 2
 *   ln link_{N+3}                       N + 3
 *
 * so the final count is N + 3 (upstream: 3003 for N = 3000) and link_0002 is
 * the single name that must be absent -- upstream inverts its own check for
 * exactly that index.
 *
 * Deviation: as in generic/040, upstream's 3000 links target btrfs' extended
 * reference threshold, an on-disk detail with no NFS analogue, so the count
 * is scaled. The name-reuse sequence, which is the NFS-relevant part, is
 * reproduced exactly.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G041_ROOT	XFS_MNT "/g041"
#define G041_FOO	G041_ROOT "/foo"

#define G041_TEXT	"hello world\n"
#define G041_LEN	(sizeof(G041_TEXT) - 1)

/* upstream's 3000, scaled: see the header */
#define G041_LINKS	400
#define G041_FINAL	(G041_LINKS + 3)	/* upstream's 3003 */
#define G041_MISSING	2			/* the one name that must be gone */

static void g041_linkname(char *buf, size_t sz, int i)
{
	snprintf(buf, sz, G041_ROOT "/foo_link_%04d", i);
}

static void g041_remove_tree(void *unused)
{
	char name[80];
	int i;

	for (i = 1; i <= G041_FINAL; i++) {
		g041_linkname(name, sizeof(name), i);
		xfs_unlink(name);
	}
	xfs_unlink(G041_FOO);
	xfs_rmdir_settled(G041_ROOT);
}

static void g041_link(struct kunit *test, int i, const char *what)
{
	char name[80];

	g041_linkname(name, sizeof(name), i);
	KUNIT_ASSERT_EQ_MSG(test, xfs_link(G041_FOO, name), 0,
			    "%s: linking foo_link_%04d", what, i);
}

static void g041_unlink(struct kunit *test, int i, const char *what)
{
	char name[80];

	g041_linkname(name, sizeof(name), i);
	KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(name), 0,
			    "%s: unlinking foo_link_%04d", what, i);
}

static void a_removed_link_name_can_be_reused(struct kunit *test)
{
	char name[80];
	char got[G041_LEN];
	struct kstat st;
	ssize_t r;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G041_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g041_remove_tree,
						  NULL), 0);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G041_FOO, G041_TEXT, G041_LEN), 0);
	for (i = 1; i <= G041_LINKS; i++)
		g041_link(test, i, "setup");

	/* upstream's sync */
	KUNIT_ASSERT_EQ(test, xfs_fsync_path(G041_FOO), 0);

	/*
	 * Upstream's sequence, verbatim. The second step is the point of the
	 * test: foo_link_0001 is recreated under the name just removed.
	 */
	g041_unlink(test, 1, "step 1");
	g041_link(test, G041_LINKS + 1, "step 2");
	g041_link(test, 1, "step 3 (name reuse)");
	g041_unlink(test, G041_MISSING, "step 4");
	g041_link(test, G041_LINKS + 2, "step 5");
	g041_link(test, G041_LINKS + 3, "step 6");

	KUNIT_EXPECT_EQ_MSG(test, xfs_fsync_path(G041_FOO), 0,
			    "fsync after the link churn");

	/* "Link count: 3003" */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G041_FOO, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, (unsigned int)G041_FINAL,
			    "link count is %u, expected %d", st.nlink,
			    G041_FINAL);

	KUNIT_EXPECT_TRUE_MSG(test, xfs_exists(G041_FOO), "Link foo is missing");

	/*
	 * Upstream's name-by-name sweep, including its inverted check for the
	 * one index that was removed and never recreated. The recreated
	 * foo_link_0001 is index 1, so it must resolve again here.
	 */
	for (i = 1; i <= G041_FINAL; i++) {
		g041_linkname(name, sizeof(name), i);
		if (i == G041_MISSING)
			KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(name),
					       "Link foo_link_%04d found (it was removed)",
					       i);
		else
			KUNIT_EXPECT_TRUE_MSG(test, xfs_exists(name),
					      "Link foo_link_%04d is missing%s",
					      i,
					      i == 1 ? " -- the recreated name did not resolve"
						     : "");
	}

	/* rm foo_link_*; cat foo -- must not be ESTALE, data must survive */
	for (i = 1; i <= G041_FINAL; i++) {
		if (i == G041_MISSING)
			continue;
		g041_unlink(test, i, "teardown");
	}

	KUNIT_ASSERT_EQ(test, xfs_kstat(G041_FOO, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, 1U,
			    "link count after removing every link is %u, expected 1",
			    st.nlink);

	r = xfs_read_range(G041_FOO, got, G041_LEN, 0);
	KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)G041_LEN,
			    "read of foo returned %zd", r);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, G041_TEXT, G041_LEN), 0,
			    "foo's content changed");
}

static int g041_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g041_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g041_cases[] = {
	KUNIT_CASE_SLOW(a_removed_link_name_can_be_reused),
	{}
};

static struct kunit_suite g041_suite = {
	.name		= "xfstests/generic/041",
	.suite_init	= g041_suite_init,
	.suite_exit	= g041_suite_exit,
	.test_cases	= g041_cases,
};

kunit_test_suites(&g041_suite);

MODULE_DESCRIPTION("xfstests generic/041 over a loopback NFS mount");
MODULE_LICENSE("GPL");
