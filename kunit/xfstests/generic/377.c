// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/377 over a loopback NFS mount: listxattr with awkward
 * buffer sizes.
 *
 * Upstream sets user.foo, user.ping and user.hello on a file (chosen so
 * that the first two names together are longer than the third) and calls
 * src/listxattr six ways: no buffer at all, a nonexistent file, a buffer
 * too small for even one name, one big enough for the first name but not
 * the list, one big enough for the last name but not the first two, and
 * one comfortably large. Everything short must fail with ERANGE and
 * nothing may come back truncated.
 *
 * Over NFSv4.2 this is the LISTXATTRS operation (RFC 8276): the client
 * pages names back from the server with a cookie and copies them into the
 * caller's buffer, so "the buffer is too small" is a decision the client
 * makes after it has already talked to the server, and getting it wrong
 * means either a truncated list or a buffer overrun. The zero-length call
 * is the size probe every listxattr caller makes first.
 *
 * The names are compared as a set, as upstream's sort does. The sizes 9
 * and 11 are upstream's: strlen("user.foo") + 1 and strlen("user.hello") + 1.
 *
 * Deviation: case 2 names a missing file on the mount instead of "".
 * Upstream's ENOENT for "" comes from getname() in the syscall, before any
 * lookup; kern_path() has no such check and resolves "" to the cwd.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G377_ROOT	XFS_MNT "/g377"
#define G377_FILE	G377_ROOT "/testfile"
#define G377_MISSING	G377_ROOT "/no-such-file"

static const struct g377_xattr {
	const char	*name;
	const char	*value;
} g377_xattrs[] = {
	{ "user.foo",	"bar" },
	{ "user.ping",	"pong" },
	{ "user.hello",	"there" },
};

static void g377_remove_tree(void *unused)
{
	xfs_unlink(G377_FILE);
	xfs_rmdir_settled(G377_ROOT);
}

static bool g377_listed(const char *list, ssize_t len, const char *name)
{
	ssize_t off = 0;

	while (off < len) {
		if (!strcmp(list + off, name))
			return true;
		off += strlen(list + off) + 1;
	}
	return false;
}

static void g377_expect_all(struct kunit *test, const char *list,
			    ssize_t len, size_t total)
{
	int i;

	KUNIT_ASSERT_EQ_MSG(test, len, (ssize_t)total,
			    "listxattr returned %zd, expected %zu", len, total);
	for (i = 0; i < ARRAY_SIZE(g377_xattrs); i++)
		KUNIT_EXPECT_TRUE_MSG(test,
				      g377_listed(list, len,
						  g377_xattrs[i].name),
				      "listxattr omitted %s",
				      g377_xattrs[i].name);
}

static void listxattr_sizes_are_all_or_erange(struct kunit *test)
{
	size_t total = 0;
	char *list;
	ssize_t len;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G377_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g377_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G377_FILE, "", 0), 0);

	for (i = 0; i < ARRAY_SIZE(g377_xattrs); i++) {
		const struct g377_xattr *x = &g377_xattrs[i];
		size_t n = strlen(x->name) + 1;

		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G377_FILE, x->name, x->value,
						 strlen(x->value), 0),
				    0, "setting %s failed", x->name);
		total += n;
	}

	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);

	/* 1. no size: probe with size 0, then list into exactly that much */
	len = xfs_listxattr(G377_FILE, NULL, 0);
	KUNIT_ASSERT_EQ_MSG(test, len, (ssize_t)total,
			    "the size probe returned %zd, expected %zu", len,
			    total);
	len = xfs_listxattr(G377_FILE, list, len);
	g377_expect_all(test, list, len, total);

	/* 2. a missing file: the size probe fails with ENOENT */
	len = xfs_listxattr(G377_MISSING, NULL, 0);
	KUNIT_EXPECT_EQ_MSG(test, len, (ssize_t)-ENOENT,
			    "listxattr on a missing file returned %zd", len);

	/* 3. too small for even the shortest name */
	len = xfs_listxattr(G377_FILE, list, 1);
	KUNIT_EXPECT_EQ_MSG(test, len, (ssize_t)-ERANGE,
			    "listxattr with a 1-byte buffer returned %zd",
			    len);

	/* 4. 9: room for user.foo but not the list */
	len = xfs_listxattr(G377_FILE, list, strlen("user.foo") + 1);
	KUNIT_EXPECT_EQ_MSG(test, len, (ssize_t)-ERANGE,
			    "listxattr with room for one name returned %zd",
			    len);

	/* 5. 11: room for user.hello but not for the first two */
	len = xfs_listxattr(G377_FILE, list, strlen("user.hello") + 1);
	KUNIT_EXPECT_EQ_MSG(test, len, (ssize_t)-ERANGE,
			    "listxattr with 11 bytes returned %zd", len);

	/* 6. 500: bigger than needed */
	memset(list, 0, PAGE_SIZE);
	len = xfs_listxattr(G377_FILE, list, 500);
	g377_expect_all(test, list, len, total);
}

static int g377_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g377_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g377_cases[] = {
	KUNIT_CASE(listxattr_sizes_are_all_or_erange),
	{}
};

static struct kunit_suite g377_suite = {
	.name		= "xfstests/generic/377",
	.suite_init	= g377_suite_init,
	.suite_exit	= g377_suite_exit,
	.test_cases	= g377_cases,
};

kunit_test_suites(&g377_suite);

MODULE_DESCRIPTION("xfstests generic/377 over a loopback NFS mount");
MODULE_LICENSE("GPL");
