// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/129 over a loopback NFS mount: looptest read/write storms.
 *
 * Upstream's src/looptest re-runs seeded write/read cycles with different
 * block sizes and modes (sync each write, truncate periodically, reopen
 * per iteration). Three scaled parameter sets are ported; each write is
 * stamped and immediately read back, so a single lost or misplaced WRITE
 * fails at the very iteration it happens.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G129_ROOT	XFS_MNT "/g129"

static void g129_remove_tree(void *unused)
{
	xfs_unlink(G129_ROOT "/loop");
	xfs_rmdir(G129_ROOT);
}

static void g129_storm(struct kunit *test, int iters, u32 bs, bool sync,
		       bool trunc, bool reopen, int setno)
{
	struct rnd_state st;
	struct file *f = NULL;
	u8 *wr, *rd;
	int i;
	u32 j;

	wr = kunit_kmalloc(test, bs, GFP_KERNEL);
	rd = kunit_kmalloc(test, bs, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, wr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);

	xfs_unlink(G129_ROOT "/loop");
	prandom_seed_state(&st, setno);

	f = filp_open(G129_ROOT "/loop", O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	for (i = 0; i < iters; i++) {
		loff_t off = (prandom_u32_state(&st) % (1024 * 1024 / bs)) * bs;
		loff_t pos;

		if (reopen) {
			filp_close(f, NULL);
			f = filp_open(G129_ROOT "/loop", O_RDWR, 0);
			KUNIT_ASSERT_FALSE(test, IS_ERR(f));
		}
		for (j = 0; j < bs; j++)
			wr[j] = (u8)(i ^ j ^ setno);
		pos = off;
		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, wr, bs, &pos),
				    (ssize_t)bs, "set %d iter %d: write", setno,
				    i);
		if (sync)
			KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
		if (trunc && (i % 10 == 9))
			KUNIT_ASSERT_EQ(test,
					xfs_truncate(G129_ROOT "/loop",
						     off + bs), 0);

		pos = off;
		KUNIT_ASSERT_EQ_MSG(test, kernel_read(f, rd, bs, &pos),
				    (ssize_t)bs, "set %d iter %d: read", setno, i);
		KUNIT_ASSERT_EQ_MSG(test, memcmp(wr, rd, bs), 0,
				    "set %d iter %d: verify at %lld", setno, i,
				    off);
	}
	filp_close(f, NULL);
}

static void looptest_parameter_sets_verify_every_write(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G129_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g129_remove_tree, NULL),
			0);

	g129_storm(test, 1000, 8192, true, false, false, 1);
	g129_storm(test, 200, 102400, true, true, false, 2);
	g129_storm(test, 500, 256, false, false, true, 3);
}

static int g129_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g129_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g129_cases[] = {
	KUNIT_CASE_SLOW(looptest_parameter_sets_verify_every_write),
	{}
};

static struct kunit_suite g129_suite = {
	.name		= "xfstests/generic/129",
	.suite_init	= g129_suite_init,
	.suite_exit	= g129_suite_exit,
	.test_cases	= g129_cases,
};

kunit_test_suites(&g129_suite);

MODULE_DESCRIPTION("xfstests generic/129 over a loopback NFS mount");
MODULE_LICENSE("GPL");
