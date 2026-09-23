// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/454 over a loopback NFS mount: xattr names are byte
 * strings, not text.
 *
 * Upstream is generic/453's experiment moved from filenames to attribute
 * names: set user.<key> for a set of keys that render alike but differ
 * byte for byte, give each a different value, and require every key to
 * read back its own value. A filesystem that normalises or folds the
 * names loses one of each pair.
 *
 * Over NFSv4.2 each set is a SETXATTR and each read a GETXATTR (RFC
 * 8276), with the name carried as an opaque component -- so this checks
 * that unusual bytes survive the round trip and that two keys differing
 * only in encoding stay two keys. The values are read back through the
 * server's own copy of the file as well, because the client answers
 * GETXATTR from its xattr cache and a cache keyed on a folded name would
 * otherwise hide the bug.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G454_ROOT	XFS_MNT "/g454"
#define G454_FILE	G454_ROOT "/attrfile"
#define G454_SERVER	XFS_EXPORT "/g454/attrfile"

static const struct g454_key {
	const char	*name;		/* without the user. prefix */
	const char	*value;
} g454_keys[] = {
	{ "french_caf\xc3\xa9",			"NFC" },
	{ "french_cafe\xcc\x81",		"NFD" },
	{ "greek_\xcf\x93",			"GREEK NFC" },
	{ "greek_\xcf\x92\xcc\x81",		"GREEK NFD" },
	{ "urk\xc0\xaf" "moo",			"FAKESLASH" },
	{ "emoji_\xf0\x9f\xa6\x91",		"octopus" },
	{ "hyphens_a\xe2\x80\x90" "b",		"unicode hyphen" },
	{ "hyphens_a-b",			"ascii hyphen" },
	{ "zerojoin_moo\xe2\x80\x8d" "cow",	"with joiner" },
	{ "zerojoin_moocow",			"without joiner" },
};

static void g454_remove_tree(void *unused)
{
	xfs_unlink(G454_FILE);
	xfs_rmdir_settled(G454_ROOT);
}

static void g454_check_values(struct kunit *test, const char *path,
			      const char *which)
{
	char name[128], value[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g454_keys); i++) {
		const struct g454_key *k = &g454_keys[i];
		size_t len = strlen(k->value);
		ssize_t n;

		snprintf(name, sizeof(name), "user.%s", k->name);
		n = xfs_getxattr(path, name, value, sizeof(value));
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
				    "%s: getxattr of key %d returned %zd",
				    which, i, n);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(value, k->value, len), 0,
				    "%s: key %d holds the wrong value", which,
				    i);
	}
}

static void keys_that_look_alike_stay_different_keys(struct kunit *test)
{
	char name[128], *list;
	ssize_t len, off;
	int i, count = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G454_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g454_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G454_FILE, "", 0), 0);

	for (i = 0; i < ARRAY_SIZE(g454_keys); i++) {
		const struct g454_key *k = &g454_keys[i];

		snprintf(name, sizeof(name), "user.%s", k->name);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G454_FILE, name, k->value,
						 strlen(k->value), 0),
				    0, "setting key %d failed", i);
	}

	g454_check_values(test, G454_FILE, "client");
	g454_check_values(test, G454_SERVER, "server");

	/* and none of them collapsed into another: the list has them all */
	list = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	len = xfs_listxattr(G454_FILE, list, PAGE_SIZE);
	KUNIT_ASSERT_GT_MSG(test, len, 0, "listxattr returned %zd", len);
	for (off = 0; off < len; off += strlen(list + off) + 1)
		count++;
	KUNIT_EXPECT_EQ_MSG(test, count, (int)ARRAY_SIZE(g454_keys),
			    "listxattr returned %d names for %d keys", count,
			    (int)ARRAY_SIZE(g454_keys));
}

static int g454_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g454_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g454_cases[] = {
	KUNIT_CASE(keys_that_look_alike_stay_different_keys),
	{}
};

static struct kunit_suite g454_suite = {
	.name		= "xfstests/generic/454",
	.suite_init	= g454_suite_init,
	.suite_exit	= g454_suite_exit,
	.test_cases	= g454_cases,
};

kunit_test_suites(&g454_suite);

MODULE_DESCRIPTION("xfstests generic/454 over a loopback NFS mount");
MODULE_LICENSE("GPL");
