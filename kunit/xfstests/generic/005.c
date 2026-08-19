// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/005 over a loopback NFS mount: symlinks and ELOOP.
 *
 * The original builds a 50-deep chain of relative symlinks ending at a
 * real file, plus a self-referencing symlink, touches them all in one
 * command, and greps for "Too many levels of symbolic links". Over NFS
 * each traversal step is a LOOKUP + READLINK RPC; loop detection is the
 * VFS's, fed by what the client read from the server.
 *
 * The port pins the boundary the shell version only brushes against:
 * resolving symlink_N takes N+1 link traversals, and the VFS allows
 * MAXSYMLINKS (40) of them. So symlink_39 must still resolve to the file
 * and symlink_40 must fail with ELOOP -- as must the self-loop.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>	/* MAXSYMLINKS */

#include "xfstests_nfs_fixture.h"

#define G005_ROOT	XFS_MNT "/g005"
#define G005_CHAIN	50	/* as upstream: symlink_00 .. symlink_49 */

static const char *g005_name(char *buf, int i)
{
	snprintf(buf, 64, G005_ROOT "/symlink_%02d", i);
	return buf;
}

static void g005_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G005_CHAIN; i++)
		xfs_unlink(g005_name(buf, i));
	xfs_unlink(G005_ROOT "/symlink_self");
	xfs_unlink(G005_ROOT "/empty_file");
	xfs_rmdir(G005_ROOT);
}

/* touch(1)'s analog: open following symlinks, report the errno. */
static long g005_open_errno(const char *path)
{
	struct file *f = filp_open(path, O_RDONLY, 0);

	if (IS_ERR(f))
		return PTR_ERR(f);
	filp_close(f, NULL);
	return 0;
}

static void deep_symlink_chains_hit_eloop_at_maxsymlinks(struct kunit *test)
{
	char buf[64];
	char target[32];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G005_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g005_remove_tree,
						  NULL), 0);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G005_ROOT "/empty_file", "", 0), 0);

	/* ln -s $o $f: relative targets, resolved in the link's directory */
	strscpy(target, "empty_file", sizeof(target));
	for (i = 0; i < G005_CHAIN; i++) {
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_symlink(target, g005_name(buf, i)), 0,
				    "creating symlink_%02d over NFS failed", i);
		snprintf(target, sizeof(target), "symlink_%02d", i);
	}
	KUNIT_ASSERT_EQ(test,
			xfs_symlink("symlink_self",
				    G005_ROOT "/symlink_self"), 0);

	/* symlink_N costs N+1 traversals; MAXSYMLINKS of them are allowed */
	KUNIT_EXPECT_EQ_MSG(test,
			    g005_open_errno(g005_name(buf, MAXSYMLINKS - 1)),
			    0L,
			    "a chain of exactly MAXSYMLINKS traversals must resolve");
	KUNIT_EXPECT_EQ_MSG(test,
			    g005_open_errno(g005_name(buf, MAXSYMLINKS)),
			    (long)-ELOOP,
			    "MAXSYMLINKS+1 traversals must fail with ELOOP");
	KUNIT_EXPECT_EQ_MSG(test,
			    g005_open_errno(g005_name(buf, G005_CHAIN - 1)),
			    (long)-ELOOP,
			    "the deepest chain link must fail with ELOOP");

	/* "*** touch recusive symlinks" */
	KUNIT_EXPECT_EQ_MSG(test,
			    g005_open_errno(G005_ROOT "/symlink_self"),
			    (long)-ELOOP,
			    "a self-referencing symlink must fail with ELOOP");

	/* the shallow end of the chain still works and reaches the file */
	KUNIT_EXPECT_EQ(test, g005_open_errno(g005_name(buf, 0)), 0L);
}

static int g005_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g005_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g005_cases[] = {
	KUNIT_CASE(deep_symlink_chains_hit_eloop_at_maxsymlinks),
	{}
};

static struct kunit_suite g005_suite = {
	.name		= "xfstests/generic/005",
	.suite_init	= g005_suite_init,
	.suite_exit	= g005_suite_exit,
	.test_cases	= g005_cases,
};

kunit_test_suites(&g005_suite);

MODULE_DESCRIPTION("xfstests generic/005 over a loopback NFS mount");
MODULE_LICENSE("GPL");
