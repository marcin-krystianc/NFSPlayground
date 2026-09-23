// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/337 over a loopback NFS mount: listxattr lists every
 * xattr.
 *
 * Upstream sets five user xattrs on one file -- the first three chosen so
 * their names collide under crc32c, which made btrfs pack them into one
 * btree item and list only one of them -- and requires getfattr --dump to
 * show all five, sorted.
 *
 * Over NFSv4.2 listxattr is the LISTXATTRS operation (RFC 8276), which
 * the client pages through with a cookie, and the names come back from
 * the server rather than from any local store. The colliding names are
 * kept because they cost nothing; what they exercise here is the server's
 * tmpfs xattr list and the client's decode of it.
 *
 * Deviations: the names are compared as a set rather than in getfattr's
 * sorted order (NFS does not promise an order, and upstream's sort is
 * getfattr's, not the filesystem's), each value is read back with
 * getxattr, and the whole list is checked a second time through the
 * server's own copy of the file so that a client-side xattr cache cannot
 * answer for it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G337_ROOT	XFS_MNT "/g337"
#define G337_FILE	G337_ROOT "/testfile"
#define G337_SERVER	XFS_EXPORT "/g337/testfile"

static const struct g337_xattr {
	const char	*name;
	const char	*value;
} g337_xattrs[] = {
	{ "user.foobar",		"123" },
	{ "user.WvG1c1Td",		"qwerty" },
	{ "user.J3__T_Km3dVsW_",	"hello" },
	{ "user.something",		"pizza" },
	{ "user.ping",			"pong" },
};

static void g337_remove_tree(void *unused)
{
	xfs_unlink(G337_FILE);
	xfs_rmdir_settled(G337_ROOT);
}

/* is name somewhere in the NUL-separated list of len bytes? */
static bool g337_listed(const char *list, ssize_t len, const char *name)
{
	ssize_t off = 0;

	while (off < len) {
		if (!strcmp(list + off, name))
			return true;
		off += strlen(list + off) + 1;
	}
	return false;
}

static void g337_check_list(struct kunit *test, const char *path,
			    const char *which)
{
	char *list;
	ssize_t len;
	int i, count = 0;
	ssize_t off;

	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);

	len = xfs_listxattr(path, list, PAGE_SIZE);
	KUNIT_ASSERT_GT_MSG(test, len, 0, "%s: listxattr returned %zd", which,
			    len);

	for (i = 0; i < ARRAY_SIZE(g337_xattrs); i++)
		KUNIT_EXPECT_TRUE_MSG(test,
				      g337_listed(list, len,
						  g337_xattrs[i].name),
				      "%s: listxattr omitted %s", which,
				      g337_xattrs[i].name);

	for (off = 0; off < len; off += strlen(list + off) + 1)
		count++;
	KUNIT_EXPECT_EQ_MSG(test, count, (int)ARRAY_SIZE(g337_xattrs),
			    "%s: listxattr returned %d names", which, count);
}

static void listxattr_returns_every_name(struct kunit *test)
{
	char value[16];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G337_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g337_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G337_FILE, "", 0), 0);

	for (i = 0; i < ARRAY_SIZE(g337_xattrs); i++) {
		const struct g337_xattr *x = &g337_xattrs[i];

		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G337_FILE, x->name, x->value,
						 strlen(x->value), 0),
				    0, "setting %s failed", x->name);
	}

	g337_check_list(test, G337_FILE, "client");
	g337_check_list(test, G337_SERVER, "server");

	/* and every value still belongs to its own name */
	for (i = 0; i < ARRAY_SIZE(g337_xattrs); i++) {
		const struct g337_xattr *x = &g337_xattrs[i];
		ssize_t len = xfs_getxattr(G337_FILE, x->name, value,
					   sizeof(value));

		KUNIT_ASSERT_EQ_MSG(test, len, (ssize_t)strlen(x->value),
				    "%s: getxattr returned %zd", x->name, len);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(value, x->value, len), 0,
				    "%s holds the wrong value", x->name);
	}
}

static int g337_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g337_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g337_cases[] = {
	KUNIT_CASE(listxattr_returns_every_name),
	{}
};

static struct kunit_suite g337_suite = {
	.name		= "xfstests/generic/337",
	.suite_init	= g337_suite_init,
	.suite_exit	= g337_suite_exit,
	.test_cases	= g337_cases,
};

kunit_test_suites(&g337_suite);

MODULE_DESCRIPTION("xfstests generic/337 over a loopback NFS mount");
MODULE_LICENSE("GPL");
