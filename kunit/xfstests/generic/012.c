// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/012 over a loopback NFS mount: the collapse-range
 * matrix.
 *
 * Upstream runs common/punch's _test_generic_punch with fcollapse as the
 * operation: seventeen numbered file layouts (holes, data, unwritten
 * extents, and their transitions), a collapse over each, then a filtered
 * fiemap dump and an md5 of the result, all diffed against 012.out.
 *
 * The flag matters. Four sibling tests drive the same engine with the
 * same six arguments and differ only in flags: 021 plain, 022 "-d" (no
 * fsync, delayed allocation), 016 "-d -k", and 012 "-k". That -k clears
 * remove_testfile, so the scratch file is NOT deleted between cases --
 * each layout is built on top of the previous one's result. That is the
 * "Multi" in "Multi collapse range tests", and it is the single thing
 * distinguishing 012 from 021; upstream's own golden files differ because
 * of it (case 4 reads "0: [0..255]: extent" under -k where 021 reads
 * "0: [0..127]: hole"). This port is cumulative to match: one unlink
 * before the first layout, never again, and the shadow model carries
 * forward across cases exactly as the file does.
 *
 * Over NFS collapse has no protocol mapping -- generic/021 pins the bare
 * EOPNOTSUPP -- so what is left to port is the matrix itself: for every
 * layout NFSv4.2 can construct, the rejected collapse must be a perfect
 * no-op, byte for byte and size for size. The layouts are not props:
 * building case 11 issues real ALLOCATE and DEALLOCATE RPCs, and case 17
 * a shrinking SETATTR, before the collapse is ever attempted.
 *
 * Deviations, all structural: the unwritten-extent cases (3, 5, 7, 8, 9,
 * 12, 13) cannot be built over NFS -- unwritten extents come from
 * fallocate KEEP_SIZE, which nfs42_fallocate() rejects -- and case 16
 * differs from 14 only in page-cache temperature, which cannot matter to
 * an operation the client refuses before touching the cache. Upstream
 * itself skips the EOF case for fcollapse. Sizes use upstream's
 * multiple=16 scaling: 64K/128K/192K/320K.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G012_ROOT	XFS_MNT "/g012"
#define G012_FILE	G012_ROOT "/case"

#define G012_4K		(64 * 1024)	/* upstream's $_4k with multiple=16 */
#define G012_8K		(128 * 1024)
#define G012_12K	(192 * 1024)
#define G012_20K	(320 * 1024)

struct g012_case {
	const char	*name;		/* upstream's numbered label */
	loff_t		size;
	struct { loff_t off; loff_t len; } writes[2];
	bool		prealloc;	/* falloc 0..size first */
	struct { loff_t off; loff_t len; } punch;	/* len 0 = none */
	struct { loff_t off; loff_t len; } collapse;
	/*
	 * An offset this layout does NOT write, which therefore must still
	 * hold a previous case's data. It is what proves -k is in effect:
	 * on a fresh file (021 semantics) the same offset would read zero.
	 * -1 for layouts that define every byte themselves.
	 */
	loff_t		carry;
};

static const struct g012_case g012_cases[] = {
	{ "1. into a hole", G012_20K,
	  { }, false, { }, { G012_4K, G012_8K }, -1 },
	{ "2. into allocated space", G012_20K,
	  { { 0, G012_20K } }, false, { }, { G012_4K, G012_8K }, -1 },
	{ "4. hole -> data", G012_20K,
	  { { G012_8K, G012_8K } }, false, { }, { G012_4K, G012_8K }, 0 },
	{ "6. data -> hole", G012_20K,
	  { { 0, G012_8K } }, false, { }, { G012_4K, G012_8K }, G012_12K },
	{ "10. hole -> data -> hole", G012_20K,
	  { { G012_8K, G012_4K } }, false, { }, { G012_4K, G012_12K }, 0 },
	{ "11. data -> hole -> data", G012_20K,
	  { { 0, G012_8K }, { G012_12K, G012_8K } }, true,
	  { G012_8K, G012_4K }, { G012_4K, G012_12K }, -1 },
	{ "14. data -> hole @ 0", G012_20K,
	  { { 0, G012_20K } }, false, { }, { 0, G012_8K }, -1 },
	{ "17. data -> hole in single block file", 4096,
	  { { 0, 4096 } }, false, { }, { 128, 128 }, -1 },
};

/*
 * The model: the file's expected content and its expected size, both
 * carried across cases because -k keeps the file.
 */
static u8 *g012_shadow;
static loff_t g012_size;

static void g012_remove_tree(void *unused)
{
	xfs_unlink(G012_FILE);
	xfs_rmdir(G012_ROOT);
}

/*
 * Build one layout on top of whatever the previous case left, updating the
 * shadow in lockstep. No unlink here: -k semantics.
 */
static void g012_build(struct kunit *test, const struct g012_case *c, int ci)
{
	struct file *f;
	u8 *buf;
	int w;

	buf = kunit_kmalloc(test, G012_8K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G012_FILE, O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/*
	 * truncate: shrinking discards the tail (case 17 goes 320K -> 4K),
	 * growing exposes zeros. Model both.
	 */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G012_FILE, c->size), 0);
	if (c->size < g012_size)
		memset(g012_shadow + c->size, 0, g012_size - c->size);
	else if (c->size > g012_size)
		memset(g012_shadow + g012_size, 0, c->size - g012_size);
	g012_size = c->size;

	if (c->prealloc)
		KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, c->size), 0,
				    "case %s: ALLOCATE failed", c->name);

	for (w = 0; w < 2 && c->writes[w].len; w++) {
		loff_t off = c->writes[w].off;
		loff_t left = c->writes[w].len;

		while (left) {
			size_t n = min_t(loff_t, left, G012_8K);
			loff_t pos = off;
			size_t i;

			for (i = 0; i < n; i++)
				buf[i] = (u8)(ci * 37 + ((off + i) >> 10));
			KUNIT_ASSERT_EQ(test, kernel_write(f, buf, n, &pos),
					(ssize_t)n);
			memcpy(g012_shadow + off, buf, n);
			off += n;
			left -= n;
		}
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	if (c->punch.len) {
		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
						  FALLOC_FL_KEEP_SIZE,
						  c->punch.off, c->punch.len),
				    0, "case %s: DEALLOCATE failed", c->name);
		memset(g012_shadow + c->punch.off, 0, c->punch.len);
	}
	filp_close(f, NULL);
}

static void collapse_is_a_perfect_noop_on_every_layout(struct kunit *test)
{
	u8 *got;
	int ci;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G012_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g012_remove_tree,
						  NULL), 0);

	g012_shadow = kunit_kzalloc(test, G012_20K, GFP_KERNEL);
	got = kunit_kmalloc(test, G012_20K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g012_shadow);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	/*
	 * Upstream's one unconditional "rm -f $testfile" before case 1, so
	 * stale state from an earlier run cannot leak in. After this the
	 * file persists for the whole matrix.
	 */
	xfs_unlink(G012_FILE);
	g012_size = 0;

	for (ci = 0; ci < ARRAY_SIZE(g012_cases); ci++) {
		const struct g012_case *c = &g012_cases[ci];
		struct kstat st;
		struct file *f;
		ssize_t n;
		int err, i;

		g012_build(test, c, ci);

		f = filp_open(G012_FILE, O_RDWR, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f));
		err = vfs_fallocate(f, FALLOC_FL_COLLAPSE_RANGE,
				    c->collapse.off, c->collapse.len);
		filp_close(f, NULL);
		KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
				    "case %s: collapse returned %d, expected EOPNOTSUPP",
				    c->name, err);

		/*
		 * -k in effect: an offset this layout never wrote still
		 * carries the previous case's data. A fresh file would
		 * read zero here, so this is what separates 012 from 021.
		 */
		if (c->carry >= 0)
			KUNIT_ASSERT_NE_MSG(test, g012_shadow[c->carry], 0,
					    "case %s: offset %lld should have been inherited from a previous layout, but the model has it as a hole -- is the file being recreated per case?",
					    c->name, c->carry);

		/* the md5 analog: size and every byte unchanged */
		KUNIT_ASSERT_EQ(test, xfs_kstat(G012_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, c->size,
				    "case %s: the rejected collapse changed the size",
				    c->name);
		n = xfs_read_range(G012_FILE, got, c->size, 0);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)c->size);
		for (i = 0; i < c->size; i++)
			if (got[i] != g012_shadow[i]) {
				KUNIT_FAIL(test,
					   "case %s (cumulative #%d): byte %d is %02x, model says %02x",
					   c->name, ci, i, got[i],
					   g012_shadow[i]);
				return;
			}

		/*
		 * The read above can be served from the client's page
		 * cache; the export directory is the server's own view.
		 * Comparing there proves the layout truly reached the
		 * server before the collapse was attempted and truly
		 * survived it.
		 */
		n = xfs_read_range(XFS_EXPORT "/g012/case", got, c->size, 0);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)c->size);
		for (i = 0; i < c->size; i++)
			if (got[i] != g012_shadow[i]) {
				KUNIT_FAIL(test,
					   "case %s (cumulative #%d): SERVER byte %d is %02x, model says %02x",
					   c->name, ci, i, got[i],
					   g012_shadow[i]);
				return;
			}
	}
}

static int g012_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g012_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g012_cases_tab[] = {
	KUNIT_CASE(collapse_is_a_perfect_noop_on_every_layout),
	{}
};

static struct kunit_suite g012_suite = {
	.name		= "xfstests/generic/012",
	.suite_init	= g012_suite_init,
	.suite_exit	= g012_suite_exit,
	.test_cases	= g012_cases_tab,
};

kunit_test_suites(&g012_suite);

MODULE_DESCRIPTION("xfstests generic/012 over a loopback NFS mount");
MODULE_LICENSE("GPL");
