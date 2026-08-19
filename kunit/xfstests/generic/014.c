// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/014 over a loopback NFS mount: truncfile.
 *
 * src/truncfile alternates, 10,000 times, a block write at a random
 * offset with an ftruncate to another random offset, on one open file.
 * Upstream's only pass criterion is that the program exits 0 -- the value
 * is in what the churn provokes (block/extent bookkeeping under constant
 * size changes). Over NFS every round is a WRITE plus a SETATTR(size),
 * exercising the client's nfs_vmtruncate/writeback interaction and the
 * server's truncation.
 *
 * The port keeps upstream's write-block/truncate loop (block stamped with
 * its own offset, exactly like writeblk()), then adds what
 * _require_sparse_files implies but upstream never checks: after the
 * storm, a deterministic epilogue proves truncate-down really cut the
 * data off (growing the file back exposes zeros, not the old bytes) and
 * that the final size is what the last truncate said.
 *
 * Deviations: 2,000 rounds rather than 10,000 (documented cut), 512-byte
 * blocks over a 1 MB range as upstream's defaults.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G014_ROOT	XFS_MNT "/g014"
#define G014_FILE	G014_ROOT "/truncfile"
#define G014_BS		512
#define G014_FILESIZE	(1024 * 1024)
#define G014_COUNT	2000	/* upstream: -c 10000 */
#define G014_SEED	1

static void g014_remove_tree(void *unused)
{
	xfs_unlink(G014_FILE);
	xfs_rmdir(G014_ROOT);
}

static void write_truncate_churn_ends_consistent(struct kunit *test)
{
	struct rnd_state st;
	struct kstat kst;
	struct file *f;
	u8 *buf;
	loff_t last_trunc = 0;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G014_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g014_remove_tree,
						  NULL), 0);

	buf = kunit_kzalloc(test, G014_BS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G014_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	prandom_seed_state(&st, G014_SEED);
	for (i = 0; i < G014_COUNT; i++) {
		loff_t off = prandom_u32_state(&st) % G014_FILESIZE;
		ssize_t w;

		/* writeblk(): the block is stamped with its own offset */
		*(u64 *)buf = *(u64 *)(buf + 256) = (u64)off;
		w = kernel_write(f, buf, G014_BS, &off);
		KUNIT_ASSERT_EQ_MSG(test, w, (ssize_t)G014_BS,
				    "write %d failed: %zd", i, w);

		/* truncfile(): chop or grow to a random size */
		last_trunc = prandom_u32_state(&st) % G014_FILESIZE;
		KUNIT_ASSERT_EQ_MSG(test, xfs_truncate(G014_FILE, last_trunc),
				    0, "truncate %d failed", i);
	}
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G014_FILE, &kst), 0);
	KUNIT_EXPECT_EQ_MSG(test, kst.size, last_trunc,
			    "final size %lld does not match the last truncate %lld",
			    kst.size, last_trunc);

	/*
	 * The sparse epilogue: write a stamped block at 64K, cut the file
	 * to 4K, grow it to 128K. The range the truncate removed must read
	 * back as zeros -- old data reappearing here is the classic
	 * truncate/pagecache bug.
	 */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G014_FILE, 4096), 0);
	KUNIT_ASSERT_EQ(test, xfs_truncate(G014_FILE, 131072), 0);
	{
		u8 *rd = kunit_kmalloc(test, G014_BS, GFP_KERNEL);
		ssize_t n;
		int j;

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);
		n = xfs_read_range(G014_FILE, rd, G014_BS, 65536);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G014_BS);
		for (j = 0; j < G014_BS; j++)
			if (rd[j]) {
				KUNIT_FAIL(test,
					   "stale data at %d after truncate down/up: %02x",
					   65536 + j, rd[j]);
				break;
			}
	}
}

static int g014_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g014_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g014_cases[] = {
	KUNIT_CASE_SLOW(write_truncate_churn_ends_consistent),
	{}
};

static struct kunit_suite g014_suite = {
	.name		= "xfstests/generic/014",
	.suite_init	= g014_suite_init,
	.suite_exit	= g014_suite_exit,
	.test_cases	= g014_cases,
};

kunit_test_suites(&g014_suite);

MODULE_DESCRIPTION("xfstests generic/014 over a loopback NFS mount");
MODULE_LICENSE("GPL");
