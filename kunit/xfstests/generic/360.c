// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/360 over a loopback NFS mount: long symlink targets.
 *
 * Upstream: symlinks whose target strings approach the limit must store
 * and resolve exactly. Over NFS the target travels in SYMLINK and comes
 * back via READLINK; the port uses a 1023-byte and a ~4000-byte target
 * (self-relative "./" padding, so they resolve to a real file), checks
 * the symlink's size attribute equals the target length, and opens
 * through both.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G360_ROOT	XFS_MNT "/g360"

static char g360_target[4020];

static void g360_remove_tree(void *unused)
{
	xfs_unlink(G360_ROOT "/short_link");
	xfs_unlink(G360_ROOT "/long_link");
	xfs_unlink(G360_ROOT "/tgt");
	xfs_rmdir(G360_ROOT);
}

static void g360_build_target(size_t len)
{
	size_t pad = len - 3;	/* "tgt" at the end */
	size_t i;

	for (i = 0; i < pad; i += 2) {
		g360_target[i] = '.';
		g360_target[i + 1] = '/';
	}
	memcpy(g360_target + pad, "tgt", 4);
}

static void g360_check_link(struct kunit *test, const char *link, size_t len)
{
	struct kstat st;
	struct file *f;
	char c;
	loff_t pos = 0;

	g360_build_target(len);
	KUNIT_ASSERT_EQ_MSG(test, xfs_symlink(g360_target, link), 0,
			    "creating a %zu-byte symlink target failed", len);

	/* the symlink inode's size is the target string length */
	KUNIT_ASSERT_EQ(test, xfs_kstat(link, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)len,
			    "READLINK size %lld for a %zu-byte target",
			    st.size, len);

	/* and it resolves through all that padding to the real file */
	f = filp_open(link, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
			       "opening through the %zu-byte target: %ld", len,
			       PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_read(f, &c, 1, &pos), (ssize_t)1);
	KUNIT_EXPECT_EQ(test, c, (char)0x54);	/* 'T' */
	filp_close(f, NULL);
}

static void long_symlink_targets_round_trip(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G360_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g360_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G360_ROOT "/tgt", "T", 1), 0);

	g360_check_link(test, G360_ROOT "/short_link", 1023);
	g360_check_link(test, G360_ROOT "/long_link", 3999);
}

static int g360_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g360_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g360_cases[] = {
	KUNIT_CASE(long_symlink_targets_round_trip),
	{}
};

static struct kunit_suite g360_suite = {
	.name		= "xfstests/generic/360",
	.suite_init	= g360_suite_init,
	.suite_exit	= g360_suite_exit,
	.test_cases	= g360_cases,
};

kunit_test_suites(&g360_suite);

MODULE_DESCRIPTION("xfstests generic/360 over a loopback NFS mount");
MODULE_LICENSE("GPL");
