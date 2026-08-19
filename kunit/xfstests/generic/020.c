// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/020 over a loopback NFS mount: many and large extended attributes.
 *
 * Upstream's attr(1) exercise: many attributes on one file, larger
 * values, list completeness, and full removal. Fifty attributes each
 * carrying a distinct value, one near-4K value, every one read back and
 * listed, then removed down to an empty list -- each step a SETXATTR/
 * GETXATTR/LISTXATTRS/REMOVEXATTR RPC.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G020_ROOT	XFS_MNT "/g020"

#define G020_FILE	G020_ROOT "/many"
#define G020_COUNT	50

static void g020_remove_tree(void *unused)
{
	xfs_unlink(G020_FILE);
	xfs_rmdir(G020_ROOT);
}

static void g020_name(char *buf, int i)
{
	snprintf(buf, 32, "user.attr%02d", i);
}

static void fifty_attributes_and_a_big_one(struct kunit *test)
{
	char name[32], val[64];
	char *list, *big, *rd;
	ssize_t n;
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G020_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g020_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G020_FILE, "m", 1), 0);

	err = xfs_setxattr(G020_FILE, "user.probe", "p", 1, 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, "user.probe"), 0);

	for (i = 0; i < G020_COUNT; i++) {
		g020_name(name, i);
		snprintf(val, sizeof(val), "value-%02d-%08x", i, i * 2654435761u);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G020_FILE, name, val,
						 strlen(val), 0), 0,
				    "setting attribute %d", i);
	}

	/* every one reads back exactly */
	for (i = 0; i < G020_COUNT; i++) {
		char want[64];

		g020_name(name, i);
		snprintf(want, sizeof(want), "value-%02d-%08x", i,
			 i * 2654435761u);
		n = xfs_getxattr(G020_FILE, name, val, sizeof(val));
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)strlen(want),
				    "attribute %d length", i);
		KUNIT_ASSERT_EQ_MSG(test, memcmp(val, want, n), 0,
				    "attribute %d value", i);
	}

	/* the list carries all fifty (name lengths: 11 + NUL each) */
	list = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	n = xfs_listxattr(G020_FILE, list, 4096);
	KUNIT_ASSERT_GE(test, n, (ssize_t)(G020_COUNT * 12));

	/* one near-4K value round-trips */
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

	/* full removal drains the list to empty */
	KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, "user.big"), 0);
	for (i = 0; i < G020_COUNT; i++) {
		g020_name(name, i);
		KUNIT_ASSERT_EQ(test, xfs_removexattr(G020_FILE, name), 0);
	}
	n = xfs_listxattr(G020_FILE, list, 4096);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)0,
			    "the attribute list is not empty after removal");
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
	KUNIT_CASE(fifty_attributes_and_a_big_one),
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
