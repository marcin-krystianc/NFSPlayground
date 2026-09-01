// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/097 over a loopback NFS mount: extended attributes.
 *
 * Upstream drives setfattr/getfattr through the basic EA lifecycle in the
 * user and trusted namespaces. NFSv4.2 (RFC 8276) carries the user.*
 * namespace only, so the port covers set/get/replace/create-flags/list/
 * remove for user.* end to end (SETXATTR/GETXATTR/LISTXATTRS/REMOVEXATTR
 * RPCs against tmpfs xattrs), and pins that trusted.* has no protocol
 * mapping.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G097_ROOT	XFS_MNT "/g097"
#define G097_FILE	G097_ROOT "/foo"

static void g097_remove_tree(void *unused)
{
	xfs_unlink(G097_FILE);
	xfs_rmdir(G097_ROOT);
}

static bool g097_list_has(const char *list, ssize_t len, const char *name)
{
	const char *p = list;

	while (p < list + len) {
		if (!strcmp(p, name))
			return true;
		p += strlen(p) + 1;
	}
	return false;
}

static void user_xattrs_full_lifecycle(struct kunit *test)
{
	char val[32], list[256];
	ssize_t n;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G097_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g097_remove_tree,
						  NULL), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G097_FILE, "f", 1), 0);

	/* set two attributes */
	err = xfs_setxattr(G097_FILE, "user.colour", "beige", 5, 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test,
			   "user xattrs unsupported on this deployment (%d)",
			   err);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "setting user.colour: %d", err);
	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G097_FILE, "user.vomit", "pizza", 5, 0),
			0);

	/* read them back exactly */
	n = xfs_getxattr(G097_FILE, "user.colour", val, sizeof(val));
	KUNIT_ASSERT_EQ(test, n, (ssize_t)5);
	KUNIT_EXPECT_EQ(test, memcmp(val, "beige", 5), 0);

	/*
	 * The client answers gets from its xattr cache, so the read above
	 * can be satisfied without asking the server. The export directory
	 * is the server's own view of the same file: verifying there proves
	 * the SETXATTR actually carried the full value over the wire.
	 */
	n = xfs_getxattr(XFS_EXPORT "/g097/foo", "user.colour", val,
			 sizeof(val));
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)5,
			    "server-side value is %zd bytes, not 5", n);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(val, "beige", 5), 0,
			    "the value on the server differs from what was set");

	/* size-probe convention: zero-size get returns the length */
	n = xfs_getxattr(G097_FILE, "user.colour", NULL, 0);
	KUNIT_EXPECT_EQ(test, n, (ssize_t)5);

	/* XATTR_REPLACE on a missing name fails, XATTR_CREATE on an
	 * existing one fails */
	KUNIT_EXPECT_EQ(test,
			xfs_setxattr(G097_FILE, "user.none", "x", 1,
				     XATTR_REPLACE), -ENODATA);
	KUNIT_EXPECT_EQ(test,
			xfs_setxattr(G097_FILE, "user.colour", "x", 1,
				     XATTR_CREATE), -EEXIST);

	/* replace changes the value */
	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G097_FILE, "user.colour", "marone", 6,
				     XATTR_REPLACE), 0);
	n = xfs_getxattr(G097_FILE, "user.colour", val, sizeof(val));
	KUNIT_ASSERT_EQ(test, n, (ssize_t)6);
	KUNIT_EXPECT_EQ(test, memcmp(val, "marone", 6), 0);

	/* the list carries both names */
	n = xfs_listxattr(G097_FILE, list, sizeof(list));
	KUNIT_ASSERT_GT(test, n, (ssize_t)0);
	KUNIT_EXPECT_TRUE_MSG(test, g097_list_has(list, n, "user.colour"),
			      "user.colour missing from LISTXATTRS");
	KUNIT_EXPECT_TRUE(test, g097_list_has(list, n, "user.vomit"));

	/* remove one; it is gone from get and list, the other survives */
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G097_FILE, "user.vomit"), 0);
	KUNIT_EXPECT_EQ(test,
			xfs_getxattr(G097_FILE, "user.vomit", val,
				     sizeof(val)), (ssize_t)-ENODATA);
	KUNIT_EXPECT_EQ(test, xfs_removexattr(G097_FILE, "user.vomit"),
			-ENODATA);
	n = xfs_listxattr(G097_FILE, list, sizeof(list));
	KUNIT_ASSERT_GE(test, n, (ssize_t)0);
	KUNIT_EXPECT_FALSE(test, g097_list_has(list, n, "user.vomit"));
	KUNIT_EXPECT_TRUE(test, g097_list_has(list, n, "user.colour"));

	/* trusted.* has no NFSv4.2 mapping; only user.* travels the wire */
	err = xfs_setxattr(G097_FILE, "trusted.colour", "x", 1, 0);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "trusted.* set over NFS: expected EOPNOTSUPP, got %d",
			    err);
	KUNIT_EXPECT_EQ(test,
			xfs_getxattr(G097_FILE, "trusted.colour", val,
				     sizeof(val)), (ssize_t)-EOPNOTSUPP);
}

static int g097_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g097_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g097_cases[] = {
	KUNIT_CASE(user_xattrs_full_lifecycle),
	{}
};

static struct kunit_suite g097_suite = {
	.name		= "xfstests/generic/097",
	.suite_init	= g097_suite_init,
	.suite_exit	= g097_suite_exit,
	.test_cases	= g097_cases,
};

kunit_test_suites(&g097_suite);

MODULE_DESCRIPTION("xfstests generic/097 over a loopback NFS mount");
MODULE_LICENSE("GPL");
