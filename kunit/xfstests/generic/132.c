// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/132 over a loopback NFS mount: pattern rewrite cycles.
 *
 * Upstream writes eight distinct 512-byte patterns back to back, reads
 * each back, then rewrites and re-verifies in cycles. Small sub-page
 * writes at adjacent offsets are exactly where a partial-page writeback
 * bug in the client shows: fifty full rewrite/verify passes here, each
 * with a different pattern generation.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G132_ROOT	XFS_MNT "/g132"

#define G132_BLKS	8
#define G132_BS		512
#define G132_PASSES	50

static void g132_remove_tree(void *unused)
{
	xfs_unlink(G132_ROOT "/pat");
	xfs_rmdir(G132_ROOT);
}

static void subpage_patterns_survive_rewrite_cycles(struct kunit *test)
{
	struct file *f;
	u8 *buf;
	int pass, blk, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G132_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g132_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G132_BS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G132_ROOT "/pat", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	for (pass = 0; pass < G132_PASSES; pass++) {
		for (blk = 0; blk < G132_BLKS; blk++) {
			loff_t pos = blk * G132_BS;

			memset(buf, 0x63 + blk + pass, G132_BS);
			KUNIT_ASSERT_EQ(test,
					kernel_write(f, buf, G132_BS, &pos),
					(ssize_t)G132_BS);
		}
		if (pass % 10 == 9)
			KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
		for (blk = 0; blk < G132_BLKS; blk++) {
			loff_t pos = blk * G132_BS;

			KUNIT_ASSERT_EQ(test,
					kernel_read(f, buf, G132_BS, &pos),
					(ssize_t)G132_BS);
			for (i = 0; i < G132_BS; i++)
				if (buf[i] != (u8)(0x63 + blk + pass)) {
					KUNIT_FAIL(test,
						   "pass %d block %d byte %d",
						   pass, blk, i);
					return;
				}
		}
	}
	filp_close(f, NULL);
}

static int g132_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g132_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g132_cases[] = {
	KUNIT_CASE(subpage_patterns_survive_rewrite_cycles),
	{}
};

static struct kunit_suite g132_suite = {
	.name		= "xfstests/generic/132",
	.suite_init	= g132_suite_init,
	.suite_exit	= g132_suite_exit,
	.test_cases	= g132_cases,
};

kunit_test_suites(&g132_suite);

MODULE_DESCRIPTION("xfstests generic/132 over a loopback NFS mount");
MODULE_LICENSE("GPL");
