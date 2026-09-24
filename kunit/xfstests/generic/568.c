// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/568 over a loopback NFS mount: fallocating an
 * unaligned range allocates every block it touches.
 *
 * Upstream fallocates two bytes at block_size - 1, so the range straddles
 * a block boundary, then writes those two bytes and requires the file's
 * allocated size not to grow. If the filesystem had only allocated one of
 * the two blocks, the write would have to allocate the other and the
 * space accounting would move.
 *
 * Over NFSv4.2 the allocation is an ALLOCATE for the same two bytes and
 * the accounting comes back as the space_used attribute, which the client
 * turns into st_blocks. So the port is really checking two things at
 * once: that the server rounds an unaligned ALLOCATE out to whole blocks,
 * and that the client reports its space_used faithfully rather than
 * deriving blocks from the size.
 *
 * Deviations: the block size is taken from the mount's st_blksize like
 * upstream's _get_file_block_size, but clamped to a page, since over NFS
 * st_blksize is the negotiated transfer size (up to a megabyte) rather
 * than any allocation unit.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G568_ROOT	XFS_MNT "/g568"
#define G568_FILE	G568_ROOT "/falloctest-568"

static void g568_remove_tree(void *unused)
{
	xfs_unlink(G568_FILE);
	xfs_rmdir_settled(G568_ROOT);
}

static void an_unaligned_fallocate_covers_both_blocks(struct kunit *test)
{
	struct kstat before, after;
	loff_t blocksize, off;
	struct file *f;
	loff_t pos;
	char two[2] = { 'a', 'b' };

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G568_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g568_remove_tree, NULL),
			0);

	f = filp_open(G568_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	KUNIT_ASSERT_EQ(test, xfs_kstat(G568_FILE, &before), 0);
	blocksize = min_t(loff_t, before.blksize, PAGE_SIZE);
	KUNIT_ASSERT_GT(test, blocksize, 1LL);
	off = blocksize - 1;

	KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, off, 2), 0,
			    "falloc %lld 2 failed", off);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G568_FILE, &before), 0);
	KUNIT_EXPECT_EQ_MSG(test, before.size, off + 2,
			    "the size after ALLOCATE is %lld", before.size);
	KUNIT_EXPECT_GT_MSG(test, (u64)before.blocks, 0ULL,
			    "the ALLOCATE reserved no space at all");

	pos = off;
	KUNIT_ASSERT_EQ(test, kernel_write(f, two, 2, &pos), 2L);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G568_FILE, &after), 0);
	KUNIT_EXPECT_LE_MSG(test, (u64)after.blocks, (u64)before.blocks,
			    "the file grew from %llu to %llu blocks when the fallocated range was written",
			    (u64)before.blocks, (u64)after.blocks);
}

static int g568_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g568_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g568_cases[] = {
	KUNIT_CASE(an_unaligned_fallocate_covers_both_blocks),
	{}
};

static struct kunit_suite g568_suite = {
	.name		= "xfstests/generic/568",
	.suite_init	= g568_suite_init,
	.suite_exit	= g568_suite_exit,
	.test_cases	= g568_cases,
};

kunit_test_suites(&g568_suite);

MODULE_DESCRIPTION("xfstests generic/568 over a loopback NFS mount");
MODULE_LICENSE("GPL");
