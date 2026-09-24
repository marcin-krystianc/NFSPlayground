// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/611 over a loopback NFS mount: an xattr whose name is
 * one character and whose value is empty.
 *
 * Upstream runs "setfattr -n user.a" (no -v, so a zero-length value),
 * cycles the mount, and requires getfattr to still find user.a. It is a
 * regression test for an XFS shortform boundary check that rejected the
 * smallest possible attribute on the next mount.
 *
 * Over NFSv4.2 the zero-length value is the interesting half: SETXATTR
 * carries an opaque value of length zero, and both the client's encoder
 * and the server have to treat that as "an attribute exists with no
 * value" rather than as a removal or an error. The port checks the value
 * is exactly zero bytes and that the name is listed, on the client and
 * again through the server's own copy, which is what upstream's mount
 * cycle stands in for here.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G611_ROOT	XFS_MNT "/g611"
#define G611_FILE	G611_ROOT "/testfile"
#define G611_SERVER	XFS_EXPORT "/g611/testfile"
#define G611_NAME	"user.a"

static void g611_remove_tree(void *unused)
{
	xfs_unlink(G611_FILE);
	xfs_rmdir_settled(G611_ROOT);
}

static void g611_check(struct kunit *test, const char *path, const char *which)
{
	char *list;
	ssize_t n, off;
	bool found = false;

	n = xfs_getxattr(path, G611_NAME, NULL, 0);
	KUNIT_EXPECT_EQ_MSG(test, n, 0L,
			    "%s: getxattr of " G611_NAME " returned %zd, expected 0",
			    which, n);

	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	n = xfs_listxattr(path, list, PAGE_SIZE);
	KUNIT_ASSERT_GT_MSG(test, n, 0, "%s: listxattr returned %zd", which, n);
	for (off = 0; off < n; off += strlen(list + off) + 1)
		if (!strcmp(list + off, G611_NAME))
			found = true;
	KUNIT_EXPECT_TRUE_MSG(test, found, "%s: " G611_NAME " is not listed",
			      which);
}

static void a_one_character_name_with_an_empty_value_survives(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G611_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g611_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G611_FILE, "", 0), 0);

	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G611_FILE, G611_NAME, "", 0, 0), 0,
			    "setting " G611_NAME " with an empty value failed");

	g611_check(test, G611_FILE, "client");
	g611_check(test, G611_SERVER, "server");
}

static int g611_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g611_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g611_cases[] = {
	KUNIT_CASE(a_one_character_name_with_an_empty_value_survives),
	{}
};

static struct kunit_suite g611_suite = {
	.name		= "xfstests/generic/611",
	.suite_init	= g611_suite_init,
	.suite_exit	= g611_suite_exit,
	.test_cases	= g611_cases,
};

kunit_test_suites(&g611_suite);

MODULE_DESCRIPTION("xfstests generic/611 over a loopback NFS mount");
MODULE_LICENSE("GPL");
