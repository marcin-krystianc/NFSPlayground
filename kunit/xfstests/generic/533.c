// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/533 over a loopback NFS mount: a user xattr smoke test.
 *
 * Upstream's sequence on one file: no attributes to start with, set
 * user.NOISE, user.COLOUR and user.SIZE, list them, cycle the mount and
 * list them again, remove user.COLOUR, list again, fetch each value
 * individually, and fetch the removed one -- which must report "No such
 * attribute". It is generic/097 without the trusted namespace, so unlike
 * 097 it runs on NFS.
 *
 * Over NFSv4.2 each step is its own operation (RFC 8276): SETXATTR,
 * LISTXATTRS, GETXATTR, REMOVEXATTR. The client caches attributes, so
 * every check here is made twice -- once through the client and once
 * against the server's own copy through the tmpfs export -- which is
 * what upstream's mount cycle is for and what the 020/037/070 ports found
 * to be necessary: a GETXATTR answered from the client's cache proves
 * nothing about what the server stored.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G533_ROOT	XFS_MNT "/g533"
#define G533_FILE	G533_ROOT "/foo.533"
#define G533_SERVER	XFS_EXPORT "/g533/foo.533"

static void g533_remove_tree(void *unused)
{
	xfs_unlink(G533_FILE);
	xfs_rmdir_settled(G533_ROOT);
}

static int g533_count(struct kunit *test, const char *path)
{
	char *list;
	ssize_t len, off;
	int count = 0;

	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	len = xfs_listxattr(path, list, PAGE_SIZE);
	if (len < 0)
		return len;
	for (off = 0; off < len; off += strlen(list + off) + 1)
		count++;
	return count;
}

static void g533_expect_value(struct kunit *test, const char *path,
			      const char *name, const char *value,
			      const char *which)
{
	char buf[32];
	ssize_t n = xfs_getxattr(path, name, buf, sizeof(buf));

	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)strlen(value),
			    "%s: %s returned %zd", which, name, n);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, value, n), 0,
			    "%s: %s holds the wrong value", which, name);
}

static void user_xattrs_are_set_listed_and_removed(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G533_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g533_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G533_FILE, "", 0), 0);

	/* a fresh file has none */
	KUNIT_EXPECT_EQ_MSG(test, g533_count(test, G533_FILE), 0,
			    "a new file already has attributes");

	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G533_FILE, "user.NOISE", "woof", 4, 0), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G533_FILE, "user.COLOUR", "blue", 4, 0), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G533_FILE, "user.SIZE", "small", 5, 0), 0);

	KUNIT_EXPECT_EQ_MSG(test, g533_count(test, G533_FILE), 3,
			    "the client does not list three attributes");
	KUNIT_EXPECT_EQ_MSG(test, g533_count(test, G533_SERVER), 3,
			    "the server does not hold three attributes");

	/* upstream's "check the list again" and its mount cycle */
	g533_expect_value(test, G533_FILE, "user.NOISE", "woof", "client");
	g533_expect_value(test, G533_SERVER, "user.NOISE", "woof", "server");
	g533_expect_value(test, G533_FILE, "user.COLOUR", "blue", "client");
	g533_expect_value(test, G533_SERVER, "user.COLOUR", "blue", "server");
	g533_expect_value(test, G533_FILE, "user.SIZE", "small", "client");
	g533_expect_value(test, G533_SERVER, "user.SIZE", "small", "server");

	KUNIT_ASSERT_EQ(test, xfs_removexattr(G533_FILE, "user.COLOUR"), 0);

	KUNIT_EXPECT_EQ_MSG(test, g533_count(test, G533_FILE), 2,
			    "the client still lists the removed attribute");
	KUNIT_EXPECT_EQ_MSG(test, g533_count(test, G533_SERVER), 2,
			    "the server still holds the removed attribute");

	g533_expect_value(test, G533_FILE, "user.NOISE", "woof", "client");
	g533_expect_value(test, G533_FILE, "user.SIZE", "small", "client");

	/* "No such attribute", on both sides */
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_getxattr(G533_FILE, "user.COLOUR", NULL, 0),
			    (ssize_t)-ENODATA,
			    "the client still answers for user.COLOUR");
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_getxattr(G533_SERVER, "user.COLOUR", NULL, 0),
			    (ssize_t)-ENODATA,
			    "the server still answers for user.COLOUR");
}

static int g533_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g533_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g533_cases[] = {
	KUNIT_CASE(user_xattrs_are_set_listed_and_removed),
	{}
};

static struct kunit_suite g533_suite = {
	.name		= "xfstests/generic/533",
	.suite_init	= g533_suite_init,
	.suite_exit	= g533_suite_exit,
	.test_cases	= g533_cases,
};

kunit_test_suites(&g533_suite);

MODULE_DESCRIPTION("xfstests generic/533 over a loopback NFS mount");
MODULE_LICENSE("GPL");
