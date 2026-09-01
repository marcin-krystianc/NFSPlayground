// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/071 over a loopback NFS mount: fallocate over preallocated space.
 *
 * Upstream preallocates a region, writes into part of it, preallocates
 * the same region again, and checks the written data survived -- a
 * regression against preallocation clobbering existing data. NFSv4.2
 * ALLOCATE (mode 0) is the supported preallocation; the port checks
 * repeated and overlapping ALLOCATEs never damage data and extend the
 * size exactly as specified.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G071_ROOT	XFS_MNT "/g071"

static void g071_remove_tree(void *unused)
{
	xfs_unlink(G071_ROOT "/f");
	xfs_rmdir(G071_ROOT);
}

static void reallocating_over_data_preserves_it(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G071_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g071_remove_tree, NULL),
			0);

	f = filp_open(G071_ROOT "/f", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* preallocate 64K: file becomes 64K of zeros */
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, 65536), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G071_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)65536);

	/* write a pattern into the first half */
	buf = kunit_kmalloc(test, 32768, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < 32768; i++)
		buf[i] = (u8)(i * 7);
	{
		loff_t pos = 0;

		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 32768, &pos),
				(ssize_t)32768);
	}

	/* preallocate the same region again, then an overlapping extension */
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, 65536), 0);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 32768, 65536), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G071_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)(32768 + 65536),
			    "overlapping ALLOCATE set the wrong size");

	n = xfs_read_range(G071_ROOT "/f", buf, 32768, 0);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)32768);
	for (i = 0; i < 32768; i++)
		if (buf[i] != (u8)(i * 7)) {
			KUNIT_FAIL(test,
				   "preallocation clobbered data at byte %d", i);
			return;
		}

	/* and the extended tail is zeros */
	n = xfs_read_range(G071_ROOT "/f", buf, 4096, 65536);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)4096);
	for (i = 0; i < 4096; i++)
		if (buf[i]) {
			KUNIT_FAIL(test, "allocated tail dirty at %d", i);
			return;
		}
}

static int g071_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g071_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g071_cases[] = {
	KUNIT_CASE(reallocating_over_data_preserves_it),
	{}
};

static struct kunit_suite g071_suite = {
	.name		= "xfstests/generic/071",
	.suite_init	= g071_suite_init,
	.suite_exit	= g071_suite_exit,
	.test_cases	= g071_cases,
};

kunit_test_suites(&g071_suite);

MODULE_DESCRIPTION("xfstests generic/071 over a loopback NFS mount");
MODULE_LICENSE("GPL");
