// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/110 over a loopback NFS mount: reflink and server-side copy.
 *
 * Upstream 110 (and the 11x reflink family) exercises clone. Two layers
 * to pin here: NFSv4.2 CLONE exists as a protocol op, but this server
 * exports tmpfs, which has no remap_file_range -- so clone fails with
 * EOPNOTSUPP end to end (protocol support cannot conjure filesystem
 * support). copy_file_range, by contrast, works: the NFSv4.2 COPY path
 * (or its generic fallback) must produce a byte-identical file. Stands
 * for the whole reflink family (111, 115-116, 118-119, 121-122, ...)
 * on this deployment.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G110_ROOT	XFS_MNT "/g110"

#define G110_SZ	(128 * 1024)

static void g110_remove_tree(void *unused)
{
	xfs_unlink(G110_ROOT "/src");
	xfs_unlink(G110_ROOT "/clone");
	xfs_unlink(G110_ROOT "/copy");
	xfs_rmdir(G110_ROOT);
}

static void clone_is_refused_but_copy_file_range_works(struct kunit *test)
{
	struct file *in, *out;
	u8 *a, *b;
	loff_t cloned;
	ssize_t copied;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G110_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g110_remove_tree, NULL),
			0);

	a = kunit_kmalloc(test, G110_SZ, GFP_KERNEL);
	b = kunit_kmalloc(test, G110_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	for (i = 0; i < G110_SZ; i++)
		a[i] = (u8)(i * 17 + (i >> 8));
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G110_ROOT "/src", a, G110_SZ),
			0);

	in = filp_open(G110_ROOT "/src", O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));

	/* CLONE: the export filesystem cannot remap, so the op must fail */
	out = filp_open(G110_ROOT "/clone", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));
	cloned = vfs_clone_file_range(in, 0, out, 0, G110_SZ, 0);
	filp_close(out, NULL);
	KUNIT_EXPECT_EQ_MSG(test, cloned, (loff_t)-EOPNOTSUPP,
			    "CLONE onto a tmpfs export: expected EOPNOTSUPP, got %lld",
			    cloned);

	/* COPY: must work and be byte-identical */
	out = filp_open(G110_ROOT "/copy", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));
	copied = vfs_copy_file_range(in, 0, out, 0, G110_SZ, 0);
	filp_close(out, NULL);
	filp_close(in, NULL);
	KUNIT_ASSERT_EQ_MSG(test, copied, (ssize_t)G110_SZ,
			    "copy_file_range moved %zd of %d bytes", copied,
			    G110_SZ);

	KUNIT_ASSERT_EQ(test, xfs_read_range(G110_ROOT "/copy", b, G110_SZ, 0),
			(ssize_t)G110_SZ);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(a, b, G110_SZ), 0,
			    "the copied file diverged from the source");
}

static int g110_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g110_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g110_cases[] = {
	KUNIT_CASE(clone_is_refused_but_copy_file_range_works),
	{}
};

static struct kunit_suite g110_suite = {
	.name		= "xfstests/generic/110",
	.suite_init	= g110_suite_init,
	.suite_exit	= g110_suite_exit,
	.test_cases	= g110_cases,
};

kunit_test_suites(&g110_suite);

MODULE_DESCRIPTION("xfstests generic/110 over a loopback NFS mount");
MODULE_LICENSE("GPL");
