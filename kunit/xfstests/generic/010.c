// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/010 over a loopback NFS mount: dbtest.
 *
 * The original drives src/dbtest, an ndbm-style keyed database over files:
 * seeded random store/fetch/delete of records, each fetch verified against
 * what the model says was stored. The database machinery is incidental --
 * what the filesystem is being asked to prove is that random-access writes
 * and reads at record granularity through one long-lived open file never
 * return anything but the last thing written.
 *
 * The port keeps that essence and drops the ndbm dressing: one file of
 * fixed-size records, a per-record generation counter as the model, and
 * seeded random store/fetch ops. Record content is regenerable from
 * (record, generation), so a fetch can be byte-verified without storing
 * the data twice. Over NFS this is thousands of interleaved WRITE/READ
 * RPCs through the same open state -- the client's dirty-page tracking
 * and read-cache coherence for a file that keeps changing beneath them.
 *
 * Deviations: 2,000 operations against 128 x 512-byte records (upstream:
 * ndbm with its own page/dir files and larger op counts); deletes are
 * modeled as store-of-generation-0 (all zeroes), since ndbm delete has no
 * per-record file analog.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G010_ROOT	XFS_MNT "/g010"
#define G010_FILE	G010_ROOT "/DBtest"
#define G010_RECS	128
#define G010_RECSZ	512
#define G010_OPS	2000
#define G010_SEED	7

static u32 g010_gen[G010_RECS];

static void g010_remove_tree(void *unused)
{
	xfs_unlink(G010_FILE);
	xfs_rmdir(G010_ROOT);
}

/* Record content: a PRNG stream keyed by (record, generation); gen 0 = zeroes. */
static void g010_fill_record(u8 *buf, int rec, u32 gen)
{
	struct rnd_state st;
	int i;
	u32 r = 0;

	if (gen == 0) {
		memset(buf, 0, G010_RECSZ);
		return;
	}
	prandom_seed_state(&st, ((u64)rec << 32) | gen);
	for (i = 0; i < G010_RECSZ; i++) {
		if ((i & 3) == 0)
			r = prandom_u32_state(&st);
		buf[i] = r >> ((i & 3) * 8);
	}
}

static void random_record_stores_and_fetches_stay_consistent(struct kunit *test)
{
	struct rnd_state st;
	struct file *f;
	u8 *buf, *want;
	int op, rec;
	ssize_t n;
	loff_t pos;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G010_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g010_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, G010_RECSZ, GFP_KERNEL);
	want = kunit_kmalloc(test, G010_RECSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, want);
	memset(g010_gen, 0, sizeof(g010_gen));

	/* the empty database: all records generation 0 (zeroes) */
	memset(buf, 0, G010_RECSZ);
	f = filp_open(G010_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (rec = 0; rec < G010_RECS; rec++) {
		pos = (loff_t)rec * G010_RECSZ;
		n = kernel_write(f, buf, G010_RECSZ, &pos);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G010_RECSZ);
	}

	prandom_seed_state(&st, G010_SEED);
	for (op = 0; op < G010_OPS; op++) {
		u32 r = prandom_u32_state(&st);

		rec = (r >> 8) % G010_RECS;
		pos = (loff_t)rec * G010_RECSZ;

		if (r % 100 < 60) {	/* store */
			g010_gen[rec]++;
			g010_fill_record(buf, rec, g010_gen[rec]);
			n = kernel_write(f, buf, G010_RECSZ, &pos);
			KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G010_RECSZ,
					    "store of record %d (op %d) short",
					    rec, op);
		} else {		/* fetch + verify */
			n = kernel_read(f, buf, G010_RECSZ, &pos);
			KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G010_RECSZ,
					    "fetch of record %d (op %d) short",
					    rec, op);
			g010_fill_record(want, rec, g010_gen[rec]);
			KUNIT_ASSERT_EQ_MSG(test, memcmp(buf, want, G010_RECSZ),
					    0,
					    "record %d (op %d) does not match generation %u",
					    rec, op, g010_gen[rec]);
		}
	}
	filp_close(f, NULL);

	/* final sweep: every record must carry its last stored generation */
	for (rec = 0; rec < G010_RECS; rec++) {
		n = xfs_read_range(G010_FILE, buf, G010_RECSZ,
				   (loff_t)rec * G010_RECSZ);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G010_RECSZ);
		g010_fill_record(want, rec, g010_gen[rec]);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, want, G010_RECSZ), 0,
				    "final sweep: record %d lost generation %u",
				    rec, g010_gen[rec]);
	}
}

static int g010_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g010_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g010_cases[] = {
	KUNIT_CASE_SLOW(random_record_stores_and_fetches_stay_consistent),
	{}
};

static struct kunit_suite g010_suite = {
	.name		= "xfstests/generic/010",
	.suite_init	= g010_suite_init,
	.suite_exit	= g010_suite_exit,
	.test_cases	= g010_cases,
};

kunit_test_suites(&g010_suite);

MODULE_DESCRIPTION("xfstests generic/010 over a loopback NFS mount");
MODULE_LICENSE("GPL");
