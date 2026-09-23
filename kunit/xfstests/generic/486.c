// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/486 over a loopback NFS mount: XATTR_REPLACE from a
 * tiny value to a large one.
 *
 * src/attr_replace_test creates user.world with a single byte using
 * XATTR_CREATE, reopens the file, and replaces it with a value of three
 * quarters of the reported block size using XATTR_REPLACE. On XFS that
 * forced a shortform-to-leaf conversion in the middle of a replace and
 * shut the filesystem down; the shell test then prints the attribute's
 * size to prove it survived.
 *
 * Over NFSv4.2 the flags are what matter: SETXATTR carries a setxattr4
 * option of EITHER (0), CREATE or REPLACE (RFC 8276), so XATTR_CREATE and
 * XATTR_REPLACE are not emulated by the client with a get-then-set --
 * they go to the server, which has to enforce them. The port therefore
 * checks the grow-in-place case upstream checks, and both error
 * directions of the two flags, which upstream does not: CREATE on a key
 * that exists is EEXIST and REPLACE on one that does not is ENODATA.
 *
 * Deviations: the large size is 2048 bytes rather than st_blksize * 3/4.
 * Over NFS st_blksize is the transfer size the mount negotiated (up to a
 * megabyte), which says nothing about the server's attribute storage, and
 * upstream itself caps the value at XATTR_SIZE_MAX for the same kind of
 * reason.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G486_ROOT	XFS_MNT "/g486"
#define G486_FILE	G486_ROOT "/hello"
#define G486_SERVER	XFS_EXPORT "/g486/hello"
#define G486_NAME	"user.world"
#define G486_BIG	2048

static void g486_remove_tree(void *unused)
{
	xfs_unlink(G486_FILE);
	xfs_rmdir_settled(G486_ROOT);
}

static void replacing_a_tiny_attr_with_a_large_one_works(struct kunit *test)
{
	char *big, *got;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G486_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g486_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G486_FILE, "", 0), 0);

	big = kunit_kmalloc(test, G486_BIG, GFP_KERNEL);
	got = kunit_kmalloc(test, G486_BIG, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, big);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	memset(big, '1', G486_BIG);

	/* a one-byte attribute, created with XATTR_CREATE */
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G486_FILE, G486_NAME, "0", 1,
					 XATTR_CREATE),
			    0, "creating the one-byte attribute failed");

	/* and the replace that used to break XFS */
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G486_FILE, G486_NAME, big, G486_BIG,
					 XATTR_REPLACE),
			    0, "replacing it with %d bytes failed", G486_BIG);

	n = xfs_getxattr(G486_FILE, G486_NAME, got, G486_BIG);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G486_BIG,
			    "the client reports a %zd byte value", n);
	for (i = 0; i < G486_BIG; i++)
		if (got[i] != '1') {
			KUNIT_FAIL(test, "byte %d of the value is %02x", i,
				   got[i]);
			break;
		}

	memset(got, 0, G486_BIG);
	n = xfs_getxattr(G486_SERVER, G486_NAME, got, G486_BIG);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G486_BIG,
			    "the server holds a %zd byte value", n);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, big, G486_BIG), 0,
			    "the server holds a different value");
}

static void the_create_and_replace_flags_are_enforced(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G486_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g486_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G486_FILE, "", 0), 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_setxattr(G486_FILE, G486_NAME, "x", 1,
					 XATTR_REPLACE),
			    -ENODATA,
			    "XATTR_REPLACE of an attribute that does not exist was allowed");

	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G486_FILE, G486_NAME, "x", 1,
				     XATTR_CREATE), 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_setxattr(G486_FILE, G486_NAME, "y", 1,
					 XATTR_CREATE),
			    -EEXIST,
			    "XATTR_CREATE of an attribute that already exists was allowed");
}

static int g486_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g486_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g486_cases[] = {
	KUNIT_CASE(replacing_a_tiny_attr_with_a_large_one_works),
	KUNIT_CASE(the_create_and_replace_flags_are_enforced),
	{}
};

static struct kunit_suite g486_suite = {
	.name		= "xfstests/generic/486",
	.suite_init	= g486_suite_init,
	.suite_exit	= g486_suite_exit,
	.test_cases	= g486_cases,
};

kunit_test_suites(&g486_suite);

MODULE_DESCRIPTION("xfstests generic/486 over a loopback NFS mount");
MODULE_LICENSE("GPL");
