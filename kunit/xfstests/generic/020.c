// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/020 over a loopback NFS mount: extended attributes.
 *
 * Upstream's attr(1)/getfattr(1) sequence on one file, step by step:
 *
 *	list a file that does not exist		ENOENT
 *	list the freshly touched file		no attributes
 *	get "nonexistant"			ENODATA
 *	set fish = "fish\n"; replace it with "fish3\n"; add snrub = "fish2\n"
 *	remove fish				only snrub is left
 *	add max_attrs attributes attribute_N = "value_N", count them,
 *	remove them all				only snrub is left
 *	set long_attr to max_attrval_size zero bytes, read it back, remove it
 *	set, get and remove a 300-character name	all fail
 *	list					only snrub
 *	delete the file
 *
 * _attr_get_max and _attr_get_maxval_size pick the sizes by FSTYP; for nfs
 * they are max_attrs=1000 and max_attrval_size=1024, used here as-is. Each
 * step is a SETXATTR/GETXATTR/LISTXATTRS/REMOVEXATTR RPC (RFC 8276).
 *
 * attr(1) rejects the 300-character name itself with EINVAL, before any
 * system call, so upstream never shows the kernel that name. Here it goes
 * down to the filesystem, where the only requirement kept is upstream's:
 * each of the three operations fails and leaves nothing behind.
 *
 * A second case, not in upstream, round-trips one near-4K value -- above
 * the 1024 bytes upstream uses for nfs, and a size an earlier version of
 * this port checked.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G020_ROOT	XFS_MNT "/g020"
#define G020_FILE	G020_ROOT "/attribute"
#define G020_MISSING	G020_ROOT "/no-such-file"
#define G020_MAX_ATTRS	1000	/* _attr_get_max, nfs */
#define G020_MAXVAL	1024	/* _attr_get_maxval_size, nfs */
#define G020_LONGNAME	300	/* $long$long$long, 10 X's x 10 x 3 */

static void g020_remove_tree(void *unused)
{
	xfs_unlink(G020_FILE);
	xfs_rmdir_settled(G020_ROOT);
}

/*
 * getfattr -d: the names and values the file holds, as one "name=value;"
 * string, so each step can be compared with what upstream's golden image
 * prints. Returns the number of attributes.
 */
static int g020_dump(struct kunit *test, char *out, size_t outlen)
{
	char *list, *val;
	ssize_t len, off, n;
	size_t used = 0;
	int count = 0;

	list = kunit_kzalloc(test, XATTR_LIST_MAX, GFP_KERNEL);
	val = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, val);
	len = xfs_listxattr(G020_FILE, list, XATTR_LIST_MAX);
	KUNIT_ASSERT_GE_MSG(test, len, 0, "listxattr returned %zd", len);
	out[0] = '\0';
	for (off = 0; off < len; off += strlen(list + off) + 1) {
		if (strncmp(list + off, "user.", 5))
			continue;	/* getfattr -d shows user.* only */
		count++;
		n = xfs_getxattr(G020_FILE, list + off, val, 63);
		if (n < 0 || used >= outlen)
			continue;
		val[n > 0 ? n : 0] = '\0';
		used += scnprintf(out + used, outlen - used, "%s=%s;",
				  list + off, val);
	}
	kunit_kfree(test, list);
	kunit_kfree(test, val);
	return count;
}

static void g020_expect_only_snrub(struct kunit *test, const char *when)
{
	char dump[128];

	KUNIT_EXPECT_EQ_MSG(test, g020_dump(test, dump, sizeof(dump)), 1,
			    "%s: the file holds %s", when, dump);
	KUNIT_EXPECT_STREQ_MSG(test, dump, "user.snrub=fish2\n;",
			       "%s", when);
}

static void the_attr_sequence_behaves_as_upstream(struct kunit *test)
{
	char name[32], val[32], dump[128];
	char *list, *longname, *big;
	ssize_t n;
	int v, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G020_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g020_remove_tree, NULL),
			0);

	/* *** list non-existant file */
	list = kunit_kzalloc(test, XATTR_LIST_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	KUNIT_EXPECT_EQ(test, xfs_listxattr(G020_MISSING, list, XATTR_LIST_MAX),
			(ssize_t)-ENOENT);

	/* *** list empty file */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G020_FILE, "", 0), 0);
	err = xfs_setxattr(G020_FILE, "user.probe", "p", 1, 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, "user.probe"), 0);
	KUNIT_EXPECT_EQ(test, g020_dump(test, dump, sizeof(dump)), 0);

	/* *** query non-existant attribute */
	KUNIT_EXPECT_EQ(test,
			xfs_getxattr(G020_FILE, "user.nonexistant", val,
				     sizeof(val)),
			(ssize_t)-ENODATA);

	/* *** one attribute: echo "fish" | attr -s fish */
	KUNIT_ASSERT_EQ(test, xfs_setxattr(G020_FILE, "user.fish", "fish\n", 5,
					   0), 0);
	KUNIT_EXPECT_EQ(test, g020_dump(test, dump, sizeof(dump)), 1);
	KUNIT_EXPECT_STREQ(test, dump, "user.fish=fish\n;");

	/* *** replace attribute */
	KUNIT_ASSERT_EQ(test, xfs_setxattr(G020_FILE, "user.fish", "fish3\n",
					   6, 0), 0);
	KUNIT_EXPECT_EQ(test, g020_dump(test, dump, sizeof(dump)), 1);
	KUNIT_EXPECT_STREQ(test, dump, "user.fish=fish3\n;");

	/* *** add attribute */
	KUNIT_ASSERT_EQ(test, xfs_setxattr(G020_FILE, "user.snrub", "fish2\n",
					   6, 0), 0);
	KUNIT_EXPECT_EQ(test, g020_dump(test, dump, sizeof(dump)), 2);
	KUNIT_EXPECT_TRUE_MSG(test,
			      !strcmp(dump, "user.fish=fish3\n;user.snrub=fish2\n;") ||
			      !strcmp(dump, "user.snrub=fish2\n;user.fish=fish3\n;"),
			      "after adding snrub the file holds %s", dump);

	/* *** remove attribute */
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, "user.fish"), 0);
	g020_expect_only_snrub(test, "after removing fish");

	/* *** add lots of attributes */
	for (v = 0; v < G020_MAX_ATTRS; v++) {
		snprintf(name, sizeof(name), "user.attribute_%d", v);
		snprintf(val, sizeof(val), "value_%d", v);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G020_FILE, name, val,
						 strlen(val), 0), 0,
				    "!!! failed to add \"attribute_%d\"", v);
	}

	/* *** check: MAX_ATTRS attribute(s) besides snrub */
	n = xfs_listxattr(G020_FILE, list, XATTR_LIST_MAX);
	KUNIT_ASSERT_GT(test, n, 0L);
	{
		ssize_t off;
		int count = 0;

		for (off = 0; off < n; off += strlen(list + off) + 1)
			if (!strncmp(list + off, "user.attribute_", 15))
				count++;
		KUNIT_EXPECT_EQ(test, count, G020_MAX_ATTRS);
	}

	/* *** remove lots of attributes */
	for (v = 0; v < G020_MAX_ATTRS; v++) {
		snprintf(name, sizeof(name), "user.attribute_%d", v);
		KUNIT_ASSERT_EQ_MSG(test, xfs_removexattr(G020_FILE, name), 0,
				    "!!! failed to remove \"attribute_%d\"", v);
	}
	g020_expect_only_snrub(test, "after removing the lots");

	/* *** really long value */
	big = kunit_kzalloc(test, G020_MAXVAL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, big);
	KUNIT_ASSERT_EQ(test, xfs_setxattr(G020_FILE, "user.long_attr", big,
					   G020_MAXVAL, 0), 0);
	memset(big, 0xff, G020_MAXVAL);
	n = xfs_getxattr(G020_FILE, "user.long_attr", big, G020_MAXVAL);
	KUNIT_EXPECT_EQ(test, n, (ssize_t)G020_MAXVAL);
	KUNIT_EXPECT_TRUE_MSG(test, !memchr_inv(big, 0, G020_MAXVAL),
			      "the long value did not read back as zeroes");
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, "user.long_attr"), 0);

	/* *** set/get/remove really long names (expect failure) */
	longname = kunit_kzalloc(test, G020_LONGNAME + 6, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, longname);
	memcpy(longname, "user.", 5);
	memset(longname + 5, 'X', G020_LONGNAME);
	err = xfs_setxattr(G020_FILE, longname, "fish", 4, 0);
	KUNIT_EXPECT_LT_MSG(test, err, 0,
			    "setting a %d-character name succeeded",
			    G020_LONGNAME);
	n = xfs_getxattr(G020_FILE, longname, val, sizeof(val));
	KUNIT_EXPECT_LT_MSG(test, n, 0L,
			    "getting a %d-character name returned %zd",
			    G020_LONGNAME, n);
	err = xfs_removexattr(G020_FILE, longname);
	KUNIT_EXPECT_LT_MSG(test, err, 0,
			    "removing a %d-character name succeeded",
			    G020_LONGNAME);

	/* *** check final */
	g020_expect_only_snrub(test, "at the end");

	/* *** delete */
	KUNIT_EXPECT_EQ(test, xfs_unlink(G020_FILE), 0);
}

/* not in upstream: a 3900-byte value round-trips intact */
static void a_near_4k_value_round_trips(struct kunit *test)
{
	char *big, *rd;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G020_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g020_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G020_FILE, "", 0), 0);
	big = kunit_kmalloc(test, 3900, GFP_KERNEL);
	rd = kunit_kmalloc(test, 3900, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, big);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);
	for (i = 0; i < 3900; i++)
		big[i] = (char)(i * 13 + 7);
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G020_FILE, "user.big", big, 3900, 0),
			    0, "a 3900-byte attribute value was refused");
	n = xfs_getxattr(G020_FILE, "user.big", rd, 3900);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)3900);
	KUNIT_EXPECT_EQ(test, memcmp(big, rd, 3900), 0);
}

static int g020_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g020_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g020_cases[] = {
	KUNIT_CASE(the_attr_sequence_behaves_as_upstream),
	KUNIT_CASE(a_near_4k_value_round_trips),
	{}
};

static struct kunit_suite g020_suite = {
	.name		= "xfstests/generic/020",
	.suite_init	= g020_suite_init,
	.suite_exit	= g020_suite_exit,
	.test_cases	= g020_cases,
};

kunit_test_suites(&g020_suite);

MODULE_DESCRIPTION("xfstests generic/020 over a loopback NFS mount");
MODULE_LICENSE("GPL");
