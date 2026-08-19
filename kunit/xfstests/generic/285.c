// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/285 over a loopback NFS mount: SEEK_HOLE and SEEK_DATA sanity.
 *
 * Upstream's seek_sanity_test. Over NFSv4.2 each llseek(SEEK_HOLE/
 * SEEK_DATA) is a SEEK RPC answered from the server's extent knowledge
 * (tmpfs is precise). Pinned: ENXIO past EOF and on empty files, the
 * virtual hole at EOF, and exact hole/data boundaries in a sparse file
 * with one 4K island at 64K.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G285_ROOT	XFS_MNT "/g285"

static void g285_remove_tree(void *unused)
{
	xfs_unlink(G285_ROOT "/sparse");
	xfs_unlink(G285_ROOT "/empty");
	xfs_rmdir(G285_ROOT);
}

static void seek_hole_data_boundaries_are_exact(struct kunit *test)
{
	struct file *f;
	u8 *buf;
	loff_t pos;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G285_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g285_remove_tree, NULL),
			0);

	/* empty file: no data, no hole to find */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G285_ROOT "/empty", "", 0), 0);
	f = filp_open(G285_ROOT "/empty", O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 0, SEEK_DATA), (loff_t)-ENXIO);
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 0, SEEK_HOLE), (loff_t)-ENXIO);
	filp_close(f, NULL);

	/* one 4K data island at 64K in an 128K file */
	buf = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xDA, 4096);
	f = filp_open(G285_ROOT "/sparse", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	pos = 65536;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 4096, &pos), (ssize_t)4096);
	KUNIT_ASSERT_EQ(test, xfs_truncate(G285_ROOT "/sparse", 131072), 0);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	/* data from the front is the island */
	KUNIT_EXPECT_EQ_MSG(test, vfs_llseek(f, 0, SEEK_DATA), (loff_t)65536,
			    "SEEK_DATA from 0 missed the island");
	/* hole from the front is immediate */
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 0, SEEK_HOLE), (loff_t)0);
	/* hole from inside the island is its end */
	KUNIT_EXPECT_EQ_MSG(test, vfs_llseek(f, 65536, SEEK_HOLE),
			    (loff_t)69632,
			    "SEEK_HOLE from the island missed its end");
	/* data past the island: nothing until EOF */
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 69632, SEEK_DATA), (loff_t)-ENXIO);
	/*
	 * An offset already inside a hole is itself the answer: SEEK_HOLE
	 * returns the smallest hole location >= offset.
	 */
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 131071, SEEK_HOLE),
			(loff_t)131071);
	/* and both refuse offsets at/past EOF */
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 131072, SEEK_DATA),
			(loff_t)-ENXIO);
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 131072, SEEK_HOLE),
			(loff_t)-ENXIO);
	filp_close(f, NULL);
}

static int g285_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g285_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g285_cases[] = {
	KUNIT_CASE(seek_hole_data_boundaries_are_exact),
	{}
};

static struct kunit_suite g285_suite = {
	.name		= "xfstests/generic/285",
	.suite_init	= g285_suite_init,
	.suite_exit	= g285_suite_exit,
	.test_cases	= g285_cases,
};

kunit_test_suites(&g285_suite);

MODULE_DESCRIPTION("xfstests generic/285 over a loopback NFS mount");
MODULE_LICENSE("GPL");
