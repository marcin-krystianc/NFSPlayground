// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/523 over a loopback NFS mount: an xattr name with a
 * slash in it.
 *
 * Upstream sets user.boo/hoo on a file and reads it back with getfattr.
 * It exists because xfs_repair once treated such a name as corruption and
 * erased it; the kernel side of the property is simply that '/' is an
 * ordinary byte in an attribute name, unlike in a path component.
 *
 * Over NFSv4.2 the name travels as a component4 in SETXATTR/GETXATTR
 * (RFC 8276), and a component is where '/' is normally forbidden -- so
 * this is a real question for the client and the server, not a formality:
 * neither may split the name or reject it. The value is checked through
 * the server's own copy as well as through the client, and listxattr has
 * to return the name whole.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G523_ROOT	XFS_MNT "/g523"
#define G523_FILE	G523_ROOT "/moofile"
#define G523_SERVER	XFS_EXPORT "/g523/moofile"
#define G523_NAME	"user.boo/hoo"
#define G523_VALUE	"woof"

static void g523_remove_tree(void *unused)
{
	xfs_unlink(G523_FILE);
	xfs_rmdir_settled(G523_ROOT);
}

static void an_xattr_name_may_contain_a_slash(struct kunit *test)
{
	char value[16], *list;
	ssize_t n;
	bool found = false;
	ssize_t off;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G523_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g523_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G523_FILE, "", 0), 0);

	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G523_FILE, G523_NAME, G523_VALUE,
					 strlen(G523_VALUE), 0),
			    0, "setting " G523_NAME " failed");

	n = xfs_getxattr(G523_FILE, G523_NAME, value, sizeof(value));
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)strlen(G523_VALUE),
			    "the client returned %zd bytes", n);
	KUNIT_EXPECT_EQ(test, memcmp(value, G523_VALUE, n), 0);

	n = xfs_getxattr(G523_SERVER, G523_NAME, value, sizeof(value));
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)strlen(G523_VALUE),
			    "the server returned %zd bytes", n);
	KUNIT_EXPECT_EQ(test, memcmp(value, G523_VALUE, n), 0);

	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	n = xfs_listxattr(G523_FILE, list, PAGE_SIZE);
	KUNIT_ASSERT_GT_MSG(test, n, 0, "listxattr returned %zd", n);
	for (off = 0; off < n; off += strlen(list + off) + 1)
		if (!strcmp(list + off, G523_NAME))
			found = true;
	KUNIT_EXPECT_TRUE_MSG(test, found,
			      "listxattr did not return " G523_NAME " whole");
}

static int g523_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g523_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g523_cases[] = {
	KUNIT_CASE(an_xattr_name_may_contain_a_slash),
	{}
};

static struct kunit_suite g523_suite = {
	.name		= "xfstests/generic/523",
	.suite_init	= g523_suite_init,
	.suite_exit	= g523_suite_exit,
	.test_cases	= g523_cases,
};

kunit_test_suites(&g523_suite);

MODULE_DESCRIPTION("xfstests generic/523 over a loopback NFS mount");
MODULE_LICENSE("GPL");
