// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/532 over a loopback NFS mount: statx attributes must
 * be inside attributes_mask.
 *
 * Upstream reads stx_attributes and stx_attributes_mask from statx and
 * requires "attrs & ~mask" to be zero: a filesystem may only report an
 * attribute bit it also claims to support. XFS once set flags without
 * ever filling in the mask.
 *
 * Over NFS the answer comes from generic_fillattr() plus whatever
 * nfs_getattr() adds, so the property is worth pinning at the point where
 * a future NFS-specific attribute (a verity or immutable bit carried in
 * an NFSv4 attribute, say) would be added without its mask bit. The port
 * checks a regular file, a directory and a symlink, since each takes a
 * different path through the client's getattr.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>

#include "xfstests_nfs_fixture.h"

#define G532_ROOT	XFS_MNT "/g532"
#define G532_FILE	G532_ROOT "/532.test"
#define G532_DIR	G532_ROOT "/532.dir"
#define G532_LINK	G532_ROOT "/532.link"

static void g532_remove_tree(void *unused)
{
	xfs_unlink(G532_LINK);
	xfs_rmdir_settled(G532_DIR);
	xfs_unlink(G532_FILE);
	xfs_rmdir_settled(G532_ROOT);
}

/* statx(2) with no sync flags, like upstream's "statx -r". kern_path()
 * with no flags does not follow a trailing symlink, so the symlink case
 * below asks about the link itself.
 */
static void g532_check(struct kunit *test, const char *path, int flags,
		       const char *what)
{
	struct kstat st;
	struct path p;
	int err;

	err = kern_path(path, flags, &p);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: kern_path: %d", what, err);
	err = vfs_getattr(&p, &st, STATX_BASIC_STATS, 0);
	path_put(&p);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: getattr: %d", what, err);

	KUNIT_EXPECT_EQ_MSG(test, st.attributes & ~st.attributes_mask, 0ULL,
			    "%s: attributes %llx do not appear in mask %llx",
			    what, st.attributes, st.attributes_mask);
}

static void reported_attributes_are_inside_the_mask(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G532_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g532_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G532_FILE, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G532_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_symlink(G532_FILE, G532_LINK), 0);

	g532_check(test, G532_FILE, 0, "a regular file");
	g532_check(test, G532_DIR, 0, "a directory");
	g532_check(test, G532_LINK, 0, "a symlink");
	g532_check(test, XFS_MNT, 0, "the mount point");
}

static int g532_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g532_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g532_cases[] = {
	KUNIT_CASE(reported_attributes_are_inside_the_mask),
	{}
};

static struct kunit_suite g532_suite = {
	.name		= "xfstests/generic/532",
	.suite_init	= g532_suite_init,
	.suite_exit	= g532_suite_exit,
	.test_cases	= g532_cases,
};

kunit_test_suites(&g532_suite);

MODULE_DESCRIPTION("xfstests generic/532 over a loopback NFS mount");
MODULE_LICENSE("GPL");
