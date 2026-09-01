// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/028 over a loopback NFS mount: path resolution stays correct across renames.
 *
 * Upstream's src/t_getcwd spins one thread calling getcwd(2) in a
 * directory while another repeatedly creates and renames a sibling file.
 * A kernel commit once made getcwd() return "/" instead of the real path;
 * the test asserts the answer never changes.
 *
 * The kernel-side equivalent of getcwd() is d_path() on a struct path, so
 * the port resolves paths that way. Single-threaded, it interleaves the
 * two halves instead of racing them: hold a deep directory, churn sibling
 * entries and rename ancestors around it, and after every operation
 * re-derive the path and require it to be exactly right -- and in
 * particular never "/", the degenerate answer the original bug produced.
 *
 * Over NFS this lands on the client's dentry parent linkage: d_path walks
 * d_parent upward, and every RENAME has to leave that chain consistent or
 * the reconstructed path is wrong. A renamed ancestor is the interesting
 * case, because the whole subtree's paths change without any of their own
 * dentries being touched.
 *
 * Deviation: no concurrency, so a genuine race is out of reach; what is
 * portable is the invariant the race was violating.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/limits.h>

#include "xfstests_nfs_fixture.h"

#define G_ROOT		XFS_MNT "/g028"

#define G_DEEP		G_ROOT "/a/b/c/d"

static void g_remove_tree(void *unused)
{
	char buf[96];
	int i;

	for (i = 0; i < 64; i++) {
		snprintf(buf, sizeof(buf), G_DEEP "/sib%d", i);
		xfs_unlink(buf);
		snprintf(buf, sizeof(buf), G_ROOT "/a/b/c2/d/sib%d", i);
		xfs_unlink(buf);
	}
	xfs_rmdir(G_ROOT "/a/b/c2/d");
	xfs_rmdir(G_ROOT "/a/b/c2");
	xfs_rmdir(G_ROOT "/a/b/c/d");
	xfs_rmdir(G_ROOT "/a/b/c");
	xfs_rmdir(G_ROOT "/a/b");
	xfs_rmdir(G_ROOT "/a");
	xfs_rmdir(G_ROOT);
}

/* getcwd()'s analog: resolve a path and render it back out */
static void g_expect_path(struct kunit *test, const char *lookup,
			  const char *expected, const char *ctx)
{
	char *buf, *rendered;
	struct path p;
	int err;

	buf = kunit_kmalloc(test, PATH_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	err = kern_path(lookup, 0, &p);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: resolving %s failed (%d)", ctx,
			    lookup, err);
	rendered = d_path(&p, buf, PATH_MAX);
	path_put(&p);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rendered),
			       "%s: d_path failed (%ld)", ctx,
			       PTR_ERR(rendered));

	/* the degenerate answer the original bug produced */
	KUNIT_ASSERT_STRNEQ_MSG(test, rendered, "/",
				"%s: d_path collapsed to \"/\"", ctx);
	KUNIT_ASSERT_STREQ_MSG(test, rendered, expected,
			       "%s: d_path gave \"%s\"", ctx, rendered);
}

static void paths_stay_correct_while_the_tree_churns(struct kunit *test)
{
	char name[96], other[96];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g_remove_tree, NULL), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a/b"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a/b/c"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_DEEP), 0);

	g_expect_path(test, G_DEEP, G_DEEP, "initial");

	/*
	 * The upstream shape: churn siblings next to the directory whose
	 * path is being resolved, re-resolving after every step.
	 */
	for (i = 0; i < 64; i++) {
		snprintf(name, sizeof(name), G_DEEP "/sib%d", i);
		snprintf(other, sizeof(other), G_DEEP "/sib%d", i + 1);

		KUNIT_ASSERT_EQ(test, xfs_write_new_file(name, "s", 1), 0);
		g_expect_path(test, G_DEEP, G_DEEP, "after create");

		KUNIT_ASSERT_EQ(test, xfs_rename(name, other), 0);
		g_expect_path(test, G_DEEP, G_DEEP, "after rename");
		/* the renamed sibling resolves to its new name, not its old */
		g_expect_path(test, other, other, "renamed sibling");

		KUNIT_ASSERT_EQ(test, xfs_unlink(other), 0);
		g_expect_path(test, G_DEEP, G_DEEP, "after unlink");
	}

	/*
	 * The harder case: rename an ancestor. Every path below it changes
	 * without any of those dentries being touched, so d_path has to be
	 * walking a parent chain the RENAME actually updated.
	 */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G_DEEP "/sib0", "s", 1), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G_ROOT "/a/b/c", G_ROOT "/a/b/c2"), 0);

	g_expect_path(test, G_ROOT "/a/b/c2/d", G_ROOT "/a/b/c2/d",
		      "after ancestor rename");
	g_expect_path(test, G_ROOT "/a/b/c2/d/sib0", G_ROOT "/a/b/c2/d/sib0",
		      "child after ancestor rename");
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G_DEEP),
			       "the old ancestor path still resolves");

	KUNIT_ASSERT_EQ(test, xfs_unlink(G_ROOT "/a/b/c2/d/sib0"), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G_ROOT "/a/b/c2", G_ROOT "/a/b/c"), 0);
	g_expect_path(test, G_DEEP, G_DEEP, "after renaming back");
}

static int g_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g_cases[] = {
	KUNIT_CASE_SLOW(paths_stay_correct_while_the_tree_churns),
	{}
};

static struct kunit_suite g_suite = {
	.name		= "xfstests/generic/028",
	.suite_init	= g_suite_init,
	.suite_exit	= g_suite_exit,
	.test_cases	= g_cases,
};

kunit_test_suites(&g_suite);

MODULE_DESCRIPTION("xfstests generic/028 over a loopback NFS mount");
MODULE_LICENSE("GPL");
