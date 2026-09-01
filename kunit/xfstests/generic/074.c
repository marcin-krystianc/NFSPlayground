// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/074 over a loopback NFS mount: fstest pattern IO.
 *
 * Upstream's src/fstest: forked workers doing patterned write/read/
 * verify rounds over a set of files. Single-threaded port: four files,
 * two hundred seeded rounds each picking a file, writing a stamped block
 * at a seeded offset, and verifying both that block and one previously
 * written block against a per-file shadow.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G074_ROOT	XFS_MNT "/g074"

#define G074_FILES	4
#define G074_FILESZ	(64 * 1024)
#define G074_ROUNDS	200
#define G074_BS		1024

static u8 *g074_shadow[G074_FILES];

static void g074_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G074_FILES; i++) {
		snprintf(buf, sizeof(buf), G074_ROOT "/t%d", i);
		xfs_unlink(buf);
	}
	xfs_rmdir(G074_ROOT);
}

static void patterned_rounds_verify_against_shadows(struct kunit *test)
{
	struct rnd_state st;
	char path[64];
	u8 *buf;
	int i, round;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G074_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g074_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G074_BS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G074_FILES; i++) {
		g074_shadow[i] = kunit_kzalloc(test, G074_FILESZ, GFP_KERNEL);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g074_shadow[i]);
		snprintf(path, sizeof(path), G074_ROOT "/t%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
		KUNIT_ASSERT_EQ(test, xfs_truncate(path, G074_FILESZ), 0);
	}

	prandom_seed_state(&st, 74);
	for (round = 0; round < G074_ROUNDS; round++) {
		u32 r = prandom_u32_state(&st);
		int f = r % G074_FILES;
		loff_t off = ((r >> 8) % (G074_FILESZ / G074_BS)) * G074_BS;
		loff_t voff = ((r >> 20) % (G074_FILESZ / G074_BS)) * G074_BS;
		ssize_t n;
		u32 j;

		snprintf(path, sizeof(path), G074_ROOT "/t%d", f);
		for (j = 0; j < G074_BS; j++)
			buf[j] = (u8)(round ^ j ^ (f << 4));

		{
			struct file *fp = filp_open(path, O_WRONLY, 0);
			loff_t pos = off;

			KUNIT_ASSERT_FALSE(test, IS_ERR(fp));
			n = kernel_write(fp, buf, G074_BS, &pos);
			filp_close(fp, NULL);
			KUNIT_ASSERT_EQ(test, n, (ssize_t)G074_BS);
		}
		memcpy(g074_shadow[f] + off, buf, G074_BS);

		/* verify the block just written and one arbitrary other */
		n = xfs_read_range(path, buf, G074_BS, off);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G074_BS);
		KUNIT_ASSERT_EQ_MSG(test,
				    memcmp(buf, g074_shadow[f] + off, G074_BS),
				    0, "round %d: fresh block diverged", round);

		n = xfs_read_range(path, buf, G074_BS, voff);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G074_BS);
		KUNIT_ASSERT_EQ_MSG(test,
				    memcmp(buf, g074_shadow[f] + voff, G074_BS),
				    0, "round %d: old block at %lld diverged",
				    round, voff);
	}
}

static int g074_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g074_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g074_cases[] = {
	KUNIT_CASE_SLOW(patterned_rounds_verify_against_shadows),
	{}
};

static struct kunit_suite g074_suite = {
	.name		= "xfstests/generic/074",
	.suite_init	= g074_suite_init,
	.suite_exit	= g074_suite_exit,
	.test_cases	= g074_cases,
};

kunit_test_suites(&g074_suite);

MODULE_DESCRIPTION("xfstests generic/074 over a loopback NFS mount");
MODULE_LICENSE("GPL");
