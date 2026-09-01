// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/075 over a loopback NFS mount: fsx.
 *
 * fsx is xfstests' random file exerciser: a shadow copy in memory, random
 * writes/truncates/punches/reads against the real file, every read
 * compared byte-for-byte against the shadow. This is a deliberately
 * reduced single-threaded fsx (no mmap, no AIO, no O_DIRECT -- which also
 * covers why 091/112/127 are not ported separately): 5000 seeded ops on a
 * file capped at 256 KB, full-file verification at the end. Over NFS,
 * every mismatch is a client cache/writeback bug by construction, since
 * the server is the only other holder of the data.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G075_ROOT	XFS_MNT "/g075"

#define G075_MAXSZ	(256 * 1024)
#define G075_OPS	5000
#define G075_SEED	75

static u8 *g075_shadow;
static loff_t g075_size;

static void g075_remove_tree(void *unused)
{
	xfs_unlink(G075_ROOT "/fsx");
	xfs_rmdir(G075_ROOT);
}

static void g075_verify_range(struct kunit *test, struct file *f,
			      loff_t off, size_t len, u8 *buf, int op)
{
	ssize_t n;
	size_t i;
	loff_t pos = off;

	if (off >= g075_size)
		return;
	if (off + (loff_t)len > g075_size)
		len = g075_size - off;
	n = kernel_read(f, buf, len, &pos);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
			    "op %d: short read at %lld", op, off);
	for (i = 0; i < len; i++)
		if (buf[i] != g075_shadow[off + i]) {
			KUNIT_FAIL(test,
				   "op %d: mismatch at %lld (file %02x shadow %02x)",
				   op, off + (loff_t)i, buf[i],
				   g075_shadow[off + i]);
			return;
		}
}

static void fsx_shadow_model_stays_in_sync(struct kunit *test)
{
	struct rnd_state st;
	struct file *f;
	u8 *buf;
	int op;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G075_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g075_remove_tree, NULL),
			0);

	g075_shadow = kunit_kzalloc(test, G075_MAXSZ, GFP_KERNEL);
	buf = kunit_kmalloc(test, G075_MAXSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g075_shadow);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	g075_size = 0;

	f = filp_open(G075_ROOT "/fsx", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	prandom_seed_state(&st, G075_SEED);
	for (op = 0; op < G075_OPS; op++) {
		u32 r = prandom_u32_state(&st);
		loff_t off = (r >> 8) % G075_MAXSZ;
		size_t len = 1 + (prandom_u32_state(&st) % 32768);
		loff_t pos;
		size_t i;

		if (off + (loff_t)len > G075_MAXSZ)
			len = G075_MAXSZ - off;

		switch (r % 10) {
		case 0: case 1: case 2: case 3:	/* pwrite */
			for (i = 0; i < len; i++)
				buf[i] = (u8)(op ^ (off + i));
			pos = off;
			KUNIT_ASSERT_EQ(test, kernel_write(f, buf, len, &pos),
					(ssize_t)len);
			/* shadow: data plus any hole the extension implies */
			if (off > g075_size)
				memset(g075_shadow + g075_size, 0,
				       off - g075_size);
			memcpy(g075_shadow + off, buf, len);
			if (off + (loff_t)len > g075_size)
				g075_size = off + len;
			break;
		case 4: case 5:			/* truncate up or down */
			KUNIT_ASSERT_EQ(test,
					xfs_truncate(G075_ROOT "/fsx", off), 0);
			if (off > g075_size)
				memset(g075_shadow + g075_size, 0,
				       off - g075_size);
			g075_size = off;
			break;
		case 6:				/* punch hole */
			if (vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
					  FALLOC_FL_KEEP_SIZE, off, len) == 0)
				if (off < g075_size)
					memset(g075_shadow + off, 0,
					       min_t(loff_t, len,
						     g075_size - off));
			break;
		default:			/* read-verify */
			g075_verify_range(test, f, off, len, buf, op);
			break;
		}
	}

	/* final: size and full content against the shadow */
	{
		struct kstat kst;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G075_ROOT "/fsx", &kst), 0);
		KUNIT_EXPECT_EQ_MSG(test, kst.size, g075_size,
				    "final size %lld, shadow says %lld",
				    kst.size, g075_size);
	}
	g075_verify_range(test, f, 0, g075_size, buf, G075_OPS);
	filp_close(f, NULL);
}

static int g075_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g075_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g075_cases[] = {
	KUNIT_CASE_SLOW(fsx_shadow_model_stays_in_sync),
	{}
};

static struct kunit_suite g075_suite = {
	.name		= "xfstests/generic/075",
	.suite_init	= g075_suite_init,
	.suite_exit	= g075_suite_exit,
	.test_cases	= g075_cases,
};

kunit_test_suites(&g075_suite);

MODULE_DESCRIPTION("xfstests generic/075 over a loopback NFS mount");
MODULE_LICENSE("GPL");
