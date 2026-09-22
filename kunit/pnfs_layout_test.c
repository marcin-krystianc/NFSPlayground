// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for pNFS layout segment bookkeeping in fs/nfs/pnfs.c.
 *
 * pnfs_test.c already covers the range arithmetic in pnfs.h (end-offset
 * saturation, intersection). This file covers what sits on top of it: the
 * decisions that manage the list of layout segments a pnfs_layout_hdr
 * holds for one inode -- which segment a lookup should return, which
 * segments a server recall should tear down, and where a new segment goes
 * in the (offset, then shortest length, then RW-over-RO) ordering the
 * client keeps that list in.
 *
 * None of this issues RPCs; it is list_for_each_entry, bit tests and
 * refcounts over a struct pnfs_layout_hdr the caller already has. That
 * makes it reachable directly, unlike the update/return paths that surround
 * it, which drive real LAYOUTGET/LAYOUTRETURN operations. docs/kunit-nfs.md
 * names pNFS as a coverage gap beyond the pnfs.h arithmetic; this narrows
 * it without needing to fake a server.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/nfs.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>

#include "nfs4_fs.h"
#include "internal.h"
#include "pnfs.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Private to pnfs.c; un-staticed by scripts/kunit/run-nfs-kunit.sh. */
s64 pnfs_lseg_range_cmp(const struct pnfs_layout_range *l1,
			const struct pnfs_layout_range *l2);
bool pnfs_lseg_range_is_after(const struct pnfs_layout_range *l1,
			      const struct pnfs_layout_range *l2);
bool pnfs_lseg_range_contained(const struct pnfs_layout_range *l1,
			       const struct pnfs_layout_range *l2);
bool pnfs_lseg_range_match(const struct pnfs_layout_range *ls_range,
			   const struct pnfs_layout_range *range,
			   bool strict_iomode);
bool pnfs_should_free_range(const struct pnfs_layout_range *lseg_range,
			    const struct pnfs_layout_range *recall_range);
bool pnfs_match_lseg_recall(const struct pnfs_layout_segment *lseg,
			    const struct pnfs_layout_range *recall_range,
			    u32 seq);
struct pnfs_layout_segment *pnfs_find_lseg(struct pnfs_layout_hdr *lo,
					   struct pnfs_layout_range *range,
					   bool strict_iomode);
bool pnfs_sanity_check_layout_range(struct pnfs_layout_range *range);
bool pnfs_within_mdsthreshold(struct nfs_open_context *ctx,
			      struct inode *ino, int iomode);

/*
 * A layout header with an empty pls_segs list and nothing else touched.
 * Every function under test here only walks that list, tests
 * NFS_LSEG_VALID/pls_flags, or reads plh_inode -- never plh_stateid,
 * plh_layouts or the refcounts that the update/return paths manage.
 */
static struct pnfs_layout_hdr *layout_hdr_new(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = kunit_kzalloc(test, sizeof(*lo),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, lo);
	INIT_LIST_HEAD(&lo->plh_segs);
	refcount_set(&lo->plh_refcount, 1);
	return lo;
}

static struct pnfs_layout_segment *
lseg_add(struct kunit *test, struct pnfs_layout_hdr *lo, u32 iomode,
	u64 offset, u64 length, u32 seq)
{
	struct pnfs_layout_segment *lseg = kunit_kzalloc(test, sizeof(*lseg),
							 GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, lseg);
	lseg->pls_range.iomode = iomode;
	lseg->pls_range.offset = offset;
	lseg->pls_range.length = length;
	lseg->pls_seq = seq;
	lseg->pls_layout = lo;
	refcount_set(&lseg->pls_refcount, 1);
	set_bit(NFS_LSEG_VALID, &lseg->pls_flags);
	list_add_tail(&lseg->pls_list, &lo->plh_segs);
	return lseg;
}

/*
 * pnfs_lseg_range_cmp() / pnfs_lseg_range_is_after(): the ordering the
 * segment list is kept in -- offset ascending, then shortest length
 * first, then RW before RO at an otherwise tied offset and length.
 */

static void higher_offset_sorts_after(struct kunit *test)
{
	struct pnfs_layout_range low = { IOMODE_RW, 0, 100 };
	struct pnfs_layout_range high = { IOMODE_RW, 100, 100 };

	KUNIT_EXPECT_LT(test, pnfs_lseg_range_cmp(&low, &high), 0);
	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_is_after(&high, &low));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_is_after(&low, &high));
}

/*
 * At the same offset, the comparator computes length2 - length1: a
 * shorter l1 makes that difference positive, so pnfs_lseg_range_cmp()
 * treats "shorter" as comparing greater -- the longer range sorts first,
 * matching the "short length > long length" comment in pnfs.c.
 */
static void longer_length_sorts_first_at_the_same_offset(struct kunit *test)
{
	struct pnfs_layout_range shorter = { IOMODE_RW, 0, 10 };
	struct pnfs_layout_range longer = { IOMODE_RW, 0, 100 };

	KUNIT_EXPECT_GT(test, pnfs_lseg_range_cmp(&shorter, &longer), 0);
	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_is_after(&shorter, &longer));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_is_after(&longer, &shorter));
}

/*
 * At the same offset and length, read-write sorts before read-only --
 * pnfs_lseg_range_cmp() returns the read-ness of l1 minus the read-ness
 * of l2, so an RW l1 (read-ness 0) against an RO l2 (read-ness 1) is
 * negative: RW compares as coming first.
 */
static void read_write_sorts_before_read_only_when_otherwise_tied(struct kunit *test)
{
	struct pnfs_layout_range rw = { IOMODE_RW, 0, 100 };
	struct pnfs_layout_range ro = { IOMODE_READ, 0, 100 };

	KUNIT_EXPECT_LT(test, pnfs_lseg_range_cmp(&rw, &ro), 0);
	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_is_after(&ro, &rw));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_is_after(&rw, &ro));
}

static void identical_ranges_compare_equal(struct kunit *test)
{
	struct pnfs_layout_range a = { IOMODE_RW, 4096, 4096 };
	struct pnfs_layout_range b = { IOMODE_RW, 4096, 4096 };

	KUNIT_EXPECT_EQ(test, pnfs_lseg_range_cmp(&a, &b), 0);
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_is_after(&a, &b));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_is_after(&b, &a));
}

/*
 * pnfs_generic_layout_insert_lseg(): walks the list looking for the first
 * existing segment the new one is not "after", and splices in before it;
 * falls to the tail if the new segment is after everything. Exported
 * (not static), so no un-staticing needed. Driven with this file's own
 * is_after/do_merge callbacks rather than the real static ones, since
 * they are supplied as function pointers -- pnfs_lseg_range_is_after
 * itself is exercised directly above.
 */

static bool never_merge(struct pnfs_layout_segment *lseg,
			struct pnfs_layout_segment *old)
{
	return false;
}

/* Returns the pls_range.offset of each segment in list order, as a string. */
static void insert_order(struct pnfs_layout_hdr *lo, char *buf, size_t len)
{
	struct pnfs_layout_segment *lseg;
	size_t n = 0;

	list_for_each_entry(lseg, &lo->plh_segs, pls_list)
		n += scnprintf(buf + n, len - n, "%llu,",
			      lseg->pls_range.offset);
}

static void insert_lseg_into_an_empty_list(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *a = lseg_add(test, lo, IOMODE_RW, 0, 0, 0);
	char order[64];

	list_del_init(&a->pls_list);	/* lseg_add() puts it on the list */

	pnfs_generic_layout_insert_lseg(lo, a, pnfs_lseg_range_is_after,
					never_merge, NULL);
	insert_order(lo, order, sizeof(order));
	KUNIT_EXPECT_STREQ(test, order, "0,");
	/* pnfs_get_layout_hdr() takes a reference on every insert. */
	KUNIT_EXPECT_EQ(test, refcount_read(&lo->plh_refcount), 2u);
}

/* A new segment is spliced in before the first existing one it precedes. */
static void insert_lseg_in_offset_order(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *mid, *low, *high;
	char order[64];

	mid = lseg_add(test, lo, IOMODE_RW, 100, 100, 0);
	list_del_init(&mid->pls_list);
	pnfs_generic_layout_insert_lseg(lo, mid, pnfs_lseg_range_is_after,
					never_merge, NULL);

	low = lseg_add(test, lo, IOMODE_RW, 0, 100, 0);
	list_del_init(&low->pls_list);
	pnfs_generic_layout_insert_lseg(lo, low, pnfs_lseg_range_is_after,
					never_merge, NULL);

	high = lseg_add(test, lo, IOMODE_RW, 200, 100, 0);
	list_del_init(&high->pls_list);
	pnfs_generic_layout_insert_lseg(lo, high, pnfs_lseg_range_is_after,
					never_merge, NULL);

	insert_order(lo, order, sizeof(order));
	KUNIT_EXPECT_STREQ(test, order, "0,100,200,");
}

/* A do_merge() that returns true retires the old segment it matched. */
static bool merge_same_offset(struct pnfs_layout_segment *lseg,
			      struct pnfs_layout_segment *old)
{
	return lseg->pls_range.offset == old->pls_range.offset;
}

static void insert_lseg_merges_and_invalidates_the_matched_segment(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *old, *new;
	LIST_HEAD(free_me);
	char order[64];

	old = lseg_add(test, lo, IOMODE_RW, 0, 100, 0);
	list_del_init(&old->pls_list);
	pnfs_generic_layout_insert_lseg(lo, old, pnfs_lseg_range_is_after,
					never_merge, NULL);

	new = lseg_add(test, lo, IOMODE_RW, 0, 200, 0);
	list_del_init(&new->pls_list);
	pnfs_generic_layout_insert_lseg(lo, new, pnfs_lseg_range_is_after,
					merge_same_offset, &free_me);

	KUNIT_EXPECT_FALSE(test, test_bit(NFS_LSEG_VALID, &old->pls_flags));
	KUNIT_EXPECT_FALSE_MSG(test, list_empty(&free_me),
			       "merged segment was not queued for freeing");
	insert_order(lo, order, sizeof(order));
	KUNIT_EXPECT_STREQ(test, order, "0,");
}

/*
 * pnfs_lseg_range_contained(): l1 contains l2 when l1's [start, end)
 * covers l2's entirely, using the same saturating end-offset as the
 * intersection test pnfs_test.c already covers.
 */

static void a_range_contains_itself(struct kunit *test)
{
	struct pnfs_layout_range r = { IOMODE_RW, 0, 100 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_contained(&r, &r));
}

static void a_wider_range_contains_a_narrower_one(struct kunit *test)
{
	struct pnfs_layout_range wide = { IOMODE_RW, 0, 1000 };
	struct pnfs_layout_range narrow = { IOMODE_RW, 100, 100 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_contained(&wide, &narrow));
	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_lseg_range_contained(&narrow, &wide),
			       "containment is not symmetric");
}

/* A range that only overlaps, without covering the other end, does not contain it. */
static void an_overlapping_but_shorter_range_does_not_contain(struct kunit *test)
{
	struct pnfs_layout_range a = { IOMODE_RW, 0, 100 };
	struct pnfs_layout_range b = { IOMODE_RW, 50, 100 };

	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_contained(&a, &b));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_contained(&b, &a));
}

/* A to-end-of-file range contains any bounded range at or after its offset. */
static void an_eof_range_contains_a_bounded_range_within_it(struct kunit *test)
{
	struct pnfs_layout_range eof = { IOMODE_RW, 0, NFS4_MAX_UINT64 };
	struct pnfs_layout_range tail = { IOMODE_RW, 1 << 20, 4096 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_contained(&eof, &tail));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_contained(&tail, &eof));
}

/*
 * pnfs_lseg_range_match(): whether an existing segment's range covers the
 * first byte of a requested range, under the RW-satisfies-any-request and
 * (optionally) exact-iomode-match rules.
 */

static void rw_segment_matches_a_read_request(struct kunit *test)
{
	struct pnfs_layout_range rw_lseg = { IOMODE_RW, 0, 1000 };
	struct pnfs_layout_range read_req = { IOMODE_READ, 0, 100 };

	KUNIT_EXPECT_TRUE(test,
			  pnfs_lseg_range_match(&rw_lseg, &read_req, false));
}

/* Under strict_iomode a read-only segment never answers a write request. */
static void a_read_only_segment_never_satisfies_a_write_request(struct kunit *test)
{
	struct pnfs_layout_range ro_lseg = { IOMODE_READ, 0, 1000 };
	struct pnfs_layout_range write_req = { IOMODE_RW, 0, 100 };

	KUNIT_EXPECT_FALSE(test,
			   pnfs_lseg_range_match(&ro_lseg, &write_req, false));
	KUNIT_EXPECT_FALSE(test,
			   pnfs_lseg_range_match(&ro_lseg, &write_req, true));
}

/*
 * With strict_iomode, a read request only matches a segment of exactly
 * the same iomode -- an RW segment does not satisfy it even though RW
 * covers reads in the non-strict case above.
 */
static void strict_iomode_requires_an_exact_read_match(struct kunit *test)
{
	struct pnfs_layout_range rw_lseg = { IOMODE_RW, 0, 1000 };
	struct pnfs_layout_range read_req = { IOMODE_READ, 0, 100 };

	KUNIT_EXPECT_FALSE(test,
			   pnfs_lseg_range_match(&rw_lseg, &read_req, true));
}

/* Only the first byte of the request has to fall inside the segment. */
static void only_the_first_byte_of_the_request_needs_to_be_covered(struct kunit *test)
{
	struct pnfs_layout_range lseg = { IOMODE_RW, 0, 10 };
	struct pnfs_layout_range req = { IOMODE_RW, 0, 1000 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_match(&lseg, &req, false));
}

/* A segment that starts after the request's first byte cannot match it. */
static void a_segment_starting_after_the_request_does_not_match(struct kunit *test)
{
	struct pnfs_layout_range lseg = { IOMODE_RW, 100, 100 };
	struct pnfs_layout_range req = { IOMODE_RW, 0, 1000 };

	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_match(&lseg, &req, false));
}

/*
 * pnfs_find_lseg(): walks plh_segs for the first valid, matching segment
 * and takes a reference on it.
 */

static void find_lseg_returns_null_on_an_empty_list(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_range range = { IOMODE_RW, 0, 100 };

	KUNIT_EXPECT_PTR_EQ(test, pnfs_find_lseg(lo, &range, false), NULL);
}

static void find_lseg_returns_the_matching_segment_with_a_reference(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *want, *got;
	struct pnfs_layout_range range = { IOMODE_RW, 0, 100 };

	want = lseg_add(test, lo, IOMODE_RW, 0, 1000, 0);

	got = pnfs_find_lseg(lo, &range, false);
	KUNIT_EXPECT_PTR_EQ(test, got, want);
	KUNIT_EXPECT_EQ(test, refcount_read(&want->pls_refcount), 2u);
}

/* An invalidated segment (NFS_LSEG_VALID cleared) is skipped, not returned. */
static void find_lseg_skips_an_invalid_segment(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *invalid, *valid;
	struct pnfs_layout_range range = { IOMODE_RW, 0, 100 };

	invalid = lseg_add(test, lo, IOMODE_RW, 0, 1000, 0);
	clear_bit(NFS_LSEG_VALID, &invalid->pls_flags);
	valid = lseg_add(test, lo, IOMODE_RW, 0, 1000, 0);

	KUNIT_EXPECT_PTR_EQ(test, pnfs_find_lseg(lo, &range, false), valid);
}

/*
 * pnfs_should_free_range() / pnfs_match_lseg_recall(): whether a server
 * recall (an iomode plus an optional range, plus a sequence barrier)
 * should tear down a given segment.
 */

static void should_free_range_requires_iomode_or_range_overlap(struct kunit *test)
{
	struct pnfs_layout_range lseg = { IOMODE_RW, 0, 100 };
	struct pnfs_layout_range recall_rw = { IOMODE_RW, 0, 100 };
	struct pnfs_layout_range recall_read = { IOMODE_READ, 0, 100 };
	struct pnfs_layout_range recall_any = { IOMODE_ANY, 200, 100 };
	struct pnfs_layout_range recall_elsewhere = { IOMODE_RW, 200, 100 };

	KUNIT_EXPECT_TRUE(test, pnfs_should_free_range(&lseg, &recall_rw));
	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_should_free_range(&lseg, &recall_read),
			       "a read recall matched an RW segment");
	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_should_free_range(&lseg, &recall_any),
			       "IOMODE_ANY matched despite no range overlap");
	KUNIT_EXPECT_FALSE(test,
			   pnfs_should_free_range(&lseg, &recall_elsewhere));
}

/* recall_range == NULL means "every segment matches", regardless of iomode. */
static void match_lseg_recall_with_no_range_matches_everything(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *lseg = lseg_add(test, lo, IOMODE_READ,
						    0, 100, 5);

	KUNIT_EXPECT_TRUE(test, pnfs_match_lseg_recall(lseg, NULL, 0));
}

/*
 * seq == 0 means "no sequence filter". A non-zero seq excludes only
 * segments whose own seq is strictly newer than it -- pnfs_seqid_is_newer()
 * is `>`, not `>=`, so a segment handed out at exactly the barrier
 * sequence is still matched, not excluded.
 */
static void match_lseg_recall_filters_by_sequence(struct kunit *test)
{
	struct pnfs_layout_hdr *lo = layout_hdr_new(test);
	struct pnfs_layout_segment *older = lseg_add(test, lo, IOMODE_RW,
						     0, 100, 5);
	struct pnfs_layout_segment *at_barrier = lseg_add(test, lo, IOMODE_RW,
							  0, 100, 10);
	struct pnfs_layout_segment *newer = lseg_add(test, lo, IOMODE_RW,
						     0, 100, 15);

	KUNIT_EXPECT_TRUE(test, pnfs_match_lseg_recall(older, NULL, 10));
	KUNIT_EXPECT_TRUE_MSG(test,
			      pnfs_match_lseg_recall(at_barrier, NULL, 10),
			      "a segment at exactly the barrier sequence was excluded");
	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_match_lseg_recall(newer, NULL, 10),
			       "a segment newer than the barrier sequence was recalled");
}

/*
 * pnfs_sanity_check_layout_range(): the bounds LAYOUTGET's reply is
 * checked against before the client trusts it.
 */

struct sanity_param {
	const char		*desc;
	struct pnfs_layout_range range;
	bool			valid;
};

static void sanity_get_desc(const struct sanity_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct sanity_param sanity_params[] = {
	{ "a bounded RW range is valid",
	  { IOMODE_RW, 0, 4096 }, true },
	{ "a bounded read range is valid",
	  { IOMODE_READ, 0, 4096 }, true },
	{ "an unbounded (to-EOF) range is valid",
	  { IOMODE_RW, 4096, NFS4_MAX_UINT64 }, true },
	{ "IOMODE_ANY is not a valid layout iomode",
	  { IOMODE_ANY, 0, 4096 }, false },
	{ "an offset of NFS4_MAX_UINT64 is invalid",
	  { IOMODE_RW, NFS4_MAX_UINT64, 100 }, false },
	{ "a zero length is invalid",
	  { IOMODE_RW, 0, 0 }, false },
	{ "a length that would overflow past the cap is invalid",
	  { IOMODE_RW, NFS4_MAX_UINT64 - 10, 20 }, false },
	{ "a length reaching exactly the cap is valid",
	  { IOMODE_RW, NFS4_MAX_UINT64 - 10, 10 }, true },
};

KUNIT_ARRAY_PARAM(sanity, sanity_params, sanity_get_desc);

static void sanity_case(struct kunit *test)
{
	const struct sanity_param *param = test->param_value;
	struct pnfs_layout_range range = param->range;

	KUNIT_EXPECT_EQ_MSG(test, pnfs_sanity_check_layout_range(&range),
			    param->valid, "%s", param->desc);
}

/*
 * pnfs_within_mdsthreshold(): should this I/O go straight to the MDS
 * instead of pNFS, based on file size and/or historical I/O size hints
 * from OPEN. Both a size test and an I/O-size test can be set at once,
 * in which case both must agree; with only one set, that one decides.
 */

static struct inode *mdsthreshold_inode_new(struct kunit *test, loff_t size)
{
	struct nfs_inode *nfsi = kunit_kzalloc(test, sizeof(*nfsi),
					       GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nfsi);
	i_size_write(&nfsi->vfs_inode, size);
	return &nfsi->vfs_inode;
}

static struct nfs_open_context *mdsthreshold_ctx_new(struct kunit *test,
						      u32 bm, u64 rd_sz,
						      u64 wr_sz, u64 rd_io_sz,
						      u64 wr_io_sz)
{
	struct nfs_open_context *ctx = kunit_kzalloc(test, sizeof(*ctx),
						     GFP_KERNEL);
	struct nfs4_threshold *t = kunit_kzalloc(test, sizeof(*t),
						 GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	t->bm = bm;
	t->rd_sz = rd_sz;
	t->wr_sz = wr_sz;
	t->rd_io_sz = rd_io_sz;
	t->wr_io_sz = wr_io_sz;
	ctx->mdsthreshold = t;
	return ctx;
}

/* No threshold set at all (t == NULL) means pNFS is always used. */
static void no_threshold_never_falls_back_to_the_mds(struct kunit *test)
{
	struct nfs_open_context ctx = {};
	struct inode *inode = mdsthreshold_inode_new(test, 1 << 30);

	KUNIT_EXPECT_FALSE(test,
			   pnfs_within_mdsthreshold(&ctx, inode, IOMODE_READ));
}

/* Below the read-size threshold, the MDS is used. */
static void small_file_read_falls_back_below_the_size_threshold(struct kunit *test)
{
	struct nfs_open_context *ctx = mdsthreshold_ctx_new(test, THRESHOLD_RD,
							    4096, 0, 0, 0);
	struct inode *inode = mdsthreshold_inode_new(test, 100);

	KUNIT_EXPECT_TRUE(test,
			  pnfs_within_mdsthreshold(ctx, inode, IOMODE_READ));
}

static void large_file_read_stays_on_pnfs_above_the_size_threshold(struct kunit *test)
{
	struct nfs_open_context *ctx = mdsthreshold_ctx_new(test, THRESHOLD_RD,
							    4096, 0, 0, 0);
	struct inode *inode = mdsthreshold_inode_new(test, 1 << 20);

	KUNIT_EXPECT_FALSE(test,
			   pnfs_within_mdsthreshold(ctx, inode, IOMODE_READ));
}

/* The write-size threshold is checked under IOMODE_RW, not IOMODE_READ. */
static void small_file_write_falls_back_below_the_size_threshold(struct kunit *test)
{
	struct nfs_open_context *ctx = mdsthreshold_ctx_new(test, THRESHOLD_WR,
							    0, 4096, 0, 0);
	struct inode *inode = mdsthreshold_inode_new(test, 100);

	KUNIT_EXPECT_TRUE(test,
			  pnfs_within_mdsthreshold(ctx, inode, IOMODE_RW));
}

/*
 * With both the size and I/O-size bits set, the two tests must agree
 * before falling back to the MDS -- one alone, below its threshold, is
 * not enough.
 */
static void both_bits_set_requires_both_to_agree(struct kunit *test)
{
	struct nfs_open_context *ctx;
	struct inode *small_file = mdsthreshold_inode_new(test, 100);

	/* Small file (falls back), but read_io already above its threshold. */
	ctx = mdsthreshold_ctx_new(test, THRESHOLD_RD | THRESHOLD_RD_IO,
				   4096, 0, 1000, 0);
	NFS_I(small_file)->read_io = 1u << 20;

	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_within_mdsthreshold(ctx, small_file,
							IOMODE_READ),
			       "one disagreeing bit still fell back to the MDS");
}

static void both_bits_set_and_agreeing_falls_back(struct kunit *test)
{
	struct nfs_open_context *ctx;
	struct inode *small_file = mdsthreshold_inode_new(test, 100);

	ctx = mdsthreshold_ctx_new(test, THRESHOLD_RD | THRESHOLD_RD_IO,
				   4096, 0, 1000, 0);
	NFS_I(small_file)->read_io = 10;

	KUNIT_EXPECT_TRUE(test,
			  pnfs_within_mdsthreshold(ctx, small_file,
						   IOMODE_READ));
}

static struct kunit_case pnfs_lseg_order_cases[] = {
	KUNIT_CASE(higher_offset_sorts_after),
	KUNIT_CASE(longer_length_sorts_first_at_the_same_offset),
	KUNIT_CASE(read_write_sorts_before_read_only_when_otherwise_tied),
	KUNIT_CASE(identical_ranges_compare_equal),
	{}
};

static struct kunit_suite pnfs_lseg_order_suite = {
	.name		= "pnfs-lseg-order",
	.test_cases	= pnfs_lseg_order_cases,
};

static struct kunit_case pnfs_lseg_insert_cases[] = {
	KUNIT_CASE(insert_lseg_into_an_empty_list),
	KUNIT_CASE(insert_lseg_in_offset_order),
	KUNIT_CASE(insert_lseg_merges_and_invalidates_the_matched_segment),
	{}
};

static struct kunit_suite pnfs_lseg_insert_suite = {
	.name		= "pnfs-lseg-insert",
	.test_cases	= pnfs_lseg_insert_cases,
};

static struct kunit_case pnfs_lseg_contain_cases[] = {
	KUNIT_CASE(a_range_contains_itself),
	KUNIT_CASE(a_wider_range_contains_a_narrower_one),
	KUNIT_CASE(an_overlapping_but_shorter_range_does_not_contain),
	KUNIT_CASE(an_eof_range_contains_a_bounded_range_within_it),
	{}
};

static struct kunit_suite pnfs_lseg_contain_suite = {
	.name		= "pnfs-lseg-contain",
	.test_cases	= pnfs_lseg_contain_cases,
};

static struct kunit_case pnfs_lseg_match_cases[] = {
	KUNIT_CASE(rw_segment_matches_a_read_request),
	KUNIT_CASE(a_read_only_segment_never_satisfies_a_write_request),
	KUNIT_CASE(strict_iomode_requires_an_exact_read_match),
	KUNIT_CASE(only_the_first_byte_of_the_request_needs_to_be_covered),
	KUNIT_CASE(a_segment_starting_after_the_request_does_not_match),
	{}
};

static struct kunit_suite pnfs_lseg_match_suite = {
	.name		= "pnfs-lseg-match",
	.test_cases	= pnfs_lseg_match_cases,
};

static struct kunit_case pnfs_find_lseg_cases[] = {
	KUNIT_CASE(find_lseg_returns_null_on_an_empty_list),
	KUNIT_CASE(find_lseg_returns_the_matching_segment_with_a_reference),
	KUNIT_CASE(find_lseg_skips_an_invalid_segment),
	{}
};

static struct kunit_suite pnfs_find_lseg_suite = {
	.name		= "pnfs-find-lseg",
	.test_cases	= pnfs_find_lseg_cases,
};

static struct kunit_case pnfs_recall_cases[] = {
	KUNIT_CASE(should_free_range_requires_iomode_or_range_overlap),
	KUNIT_CASE(match_lseg_recall_with_no_range_matches_everything),
	KUNIT_CASE(match_lseg_recall_filters_by_sequence),
	{}
};

static struct kunit_suite pnfs_recall_suite = {
	.name		= "pnfs-lseg-recall",
	.test_cases	= pnfs_recall_cases,
};

static struct kunit_case pnfs_sanity_cases[] = {
	{
		.name			= "layout range bounds",
		.run_case		= sanity_case,
		.generate_params	= sanity_gen_params,
	},
	{}
};

static struct kunit_suite pnfs_sanity_suite = {
	.name		= "pnfs-layout-range-sanity",
	.test_cases	= pnfs_sanity_cases,
};

static struct kunit_case pnfs_mdsthreshold_cases[] = {
	KUNIT_CASE(no_threshold_never_falls_back_to_the_mds),
	KUNIT_CASE(small_file_read_falls_back_below_the_size_threshold),
	KUNIT_CASE(large_file_read_stays_on_pnfs_above_the_size_threshold),
	KUNIT_CASE(small_file_write_falls_back_below_the_size_threshold),
	KUNIT_CASE(both_bits_set_requires_both_to_agree),
	KUNIT_CASE(both_bits_set_and_agreeing_falls_back),
	{}
};

static struct kunit_suite pnfs_mdsthreshold_suite = {
	.name		= "pnfs-mdsthreshold",
	.test_cases	= pnfs_mdsthreshold_cases,
};

kunit_test_suites(&pnfs_lseg_order_suite,
		  &pnfs_lseg_insert_suite,
		  &pnfs_lseg_contain_suite,
		  &pnfs_lseg_match_suite,
		  &pnfs_find_lseg_suite,
		  &pnfs_recall_suite,
		  &pnfs_sanity_suite,
		  &pnfs_mdsthreshold_suite);

MODULE_DESCRIPTION("Test pNFS layout segment list bookkeeping");
MODULE_LICENSE("GPL");
