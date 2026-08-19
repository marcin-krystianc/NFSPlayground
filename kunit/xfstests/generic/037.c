// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/037 over a loopback NFS mount: xattr value flipping.
 *
 * Upstream races a setxattr loop flipping one attribute between two
 * values against a listxattr loop, asserting a reader never sees a torn
 * state. Single-threaded port: five hundred flip iterations where after
 * every SETXATTR(REPLACE-semantics) the value read back must be exactly
 * the one just set and the list must contain exactly the one name.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G037_ROOT	XFS_MNT "/g037"

#define G037_FILE	G037_ROOT "/flip"

static void g037_remove_tree(void *unused)
{
	xfs_unlink(G037_FILE);
	xfs_rmdir(G037_ROOT);
}

static void flipping_values_are_never_torn(struct kunit *test)
{
	static const char * const vals[2] = { "foobar", "rabbit_hole" };
	char rd[32], list[64];
	ssize_t n;
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G037_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g037_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G037_FILE, "f", 1), 0);

	err = xfs_setxattr(G037_FILE, "user.something", vals[0],
			   strlen(vals[0]), 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);

	for (i = 0; i < 500; i++) {
		const char *want = vals[i & 1];

		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G037_FILE, "user.something",
						 want, strlen(want), 0), 0,
				    "flip %d failed", i);
		n = xfs_getxattr(G037_FILE, "user.something", rd, sizeof(rd));
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)strlen(want),
				    "flip %d: torn length %zd", i, n);
		KUNIT_ASSERT_EQ_MSG(test, memcmp(rd, want, n), 0,
				    "flip %d: torn value", i);

		if (i % 50 == 0) {
			n = xfs_listxattr(G037_FILE, list, sizeof(list));
			KUNIT_ASSERT_EQ_MSG(test, n,
					    (ssize_t)sizeof("user.something"),
					    "flip %d: list grew or shrank", i);
		}
	}
}

static int g037_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g037_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g037_cases[] = {
	KUNIT_CASE_SLOW(flipping_values_are_never_torn),
	{}
};

static struct kunit_suite g037_suite = {
	.name		= "xfstests/generic/037",
	.suite_init	= g037_suite_init,
	.suite_exit	= g037_suite_exit,
	.test_cases	= g037_cases,
};

kunit_test_suites(&g037_suite);

MODULE_DESCRIPTION("xfstests generic/037 over a loopback NFS mount");
MODULE_LICENSE("GPL");
