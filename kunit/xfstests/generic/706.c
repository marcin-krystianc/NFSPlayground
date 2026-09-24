// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/706 over a loopback NFS mount: SEEK_DATA on a one-byte
 * file.
 *
 * Upstream runs seek_sanity_test case 22: write one byte at offset 0,
 * SEEK_DATA from 0 (which must return 0) while the byte is still dirty,
 * fsync, and SEEK_DATA from 0 again (which must still return 0). btrfs
 * had an off-by-one in its delalloc search that made the first of those
 * two answers wrong.
 *
 * Over NFS "still dirty" means the byte is in the client's page cache and
 * has not been written back. nfs4_file_llseek() sends SEEK to the server
 * regardless, so unless the client flushes first the server is being
 * asked about a file it believes is empty -- and NFS4ERR_NXIO or an
 * answer of 1 would both be wrong. nfs42_proc_llseek()'s
 * nfs_sync_inode() before the SEEK is what makes the dirty case work, and
 * that is what this port pins.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G706_ROOT	XFS_MNT "/g706"
#define G706_FILE	G706_ROOT "/seek_sanity_testfile.706"

static void g706_remove_tree(void *unused)
{
	xfs_unlink(G706_FILE);
	xfs_rmdir_settled(G706_ROOT);
}

static void seek_data_on_a_one_byte_file(struct kunit *test)
{
	struct file *f;
	loff_t pos = 0, got;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G706_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g706_remove_tree, NULL),
			0);

	f = filp_open(G706_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, "X", 1, &pos), 1L);

	/* the byte is still only in the client's page cache */
	got = vfs_llseek(f, 0, SEEK_DATA);
	KUNIT_EXPECT_EQ_MSG(test, got, 0LL,
			    "SEEK_DATA from 0 returned %lld before writeback",
			    got);

	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	got = vfs_llseek(f, 0, SEEK_DATA);
	KUNIT_EXPECT_EQ_MSG(test, got, 0LL,
			    "SEEK_DATA from 0 returned %lld after writeback",
			    got);

	/* and the hole that follows it is at EOF */
	got = vfs_llseek(f, 0, SEEK_HOLE);
	KUNIT_EXPECT_EQ_MSG(test, got, 1LL,
			    "SEEK_HOLE from 0 returned %lld", got);

	filp_close(f, NULL);
}

static int g706_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g706_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g706_cases[] = {
	KUNIT_CASE(seek_data_on_a_one_byte_file),
	{}
};

static struct kunit_suite g706_suite = {
	.name		= "xfstests/generic/706",
	.suite_init	= g706_suite_init,
	.suite_exit	= g706_suite_exit,
	.test_cases	= g706_cases,
};

kunit_test_suites(&g706_suite);

MODULE_DESCRIPTION("xfstests generic/706 over a loopback NFS mount");
MODULE_LICENSE("GPL");
