// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/213 over a loopback NFS mount: fallocate boundary conditions.
 *
 * Upstream probes unwritten-extent boundary conditions through
 * fallocate. The NFSv4.2 ALLOCATE port covers the argument and boundary
 * space the client controls: zero and negative lengths (VFS-rejected),
 * one-byte allocations, page-boundary straddles, allocations far past
 * EOF creating a sparse tail, and that allocation never shrinks a file.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G213_ROOT	XFS_MNT "/g213"

static void g213_remove_tree(void *unused)
{
	xfs_unlink(G213_ROOT "/f");
	xfs_rmdir(G213_ROOT);
}

static void allocate_boundaries_behave(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G213_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g213_remove_tree, NULL),
			0);

	f = filp_open(G213_ROOT "/f", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* argument validation happens before any RPC */
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, -1, 4096), -EINVAL);
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 0, -4096), -EINVAL);

	/* one byte, page straddle, and far-past-EOF sparse allocations */
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 0, 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)1);

	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 4095, 2), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)4097);

	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 8 * 1024 * 1024, 4096), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)(8 * 1024 * 1024 + 4096));

	/* allocation never shrinks: covering a smaller range is a no-op */
	KUNIT_EXPECT_EQ(test, vfs_fallocate(f, 0, 0, 4096), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)(8 * 1024 * 1024 + 4096),
			    "a smaller ALLOCATE shrank the file");

	/* the sparse body reads as zeros */
	buf = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	n = kernel_read(f, buf, 4096, &(loff_t){ 1 * 1024 * 1024 });
	KUNIT_ASSERT_EQ(test, n, (ssize_t)4096);
	for (i = 0; i < 4096; i++)
		if (buf[i]) {
			KUNIT_FAIL(test, "sparse region dirty at %d", i);
			return;
		}
	filp_close(f, NULL);
}

static int g213_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g213_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g213_cases[] = {
	KUNIT_CASE(allocate_boundaries_behave),
	{}
};

static struct kunit_suite g213_suite = {
	.name		= "xfstests/generic/213",
	.suite_init	= g213_suite_init,
	.suite_exit	= g213_suite_exit,
	.test_cases	= g213_cases,
};

kunit_test_suites(&g213_suite);

MODULE_DESCRIPTION("xfstests generic/213 over a loopback NFS mount");
MODULE_LICENSE("GPL");
