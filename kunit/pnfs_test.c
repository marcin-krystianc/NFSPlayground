// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for pNFS layout range arithmetic in fs/nfs/pnfs.h.
 *
 * A pNFS layout segment covers a byte range of a file, and the client
 * constantly asks whether two ranges overlap, whether one contains
 * another, and where a range ends. The awkward part is the protocol's
 * "to end of file" convention: a length of NFS4_MAX_UINT64 means
 * unbounded, so the end-offset calculation saturates instead of wrapping
 * and the overlap test has to special-case an unbounded end.
 *
 * That is the same class of boundary arithmetic xfstests hammers with its
 * fallocate range cases, expressed here at the layout level where it can
 * be checked directly rather than inferred from I/O behaviour.
 *
 * These are static inlines in a private header, so unlike inode.c nothing
 * needs un-staticing; the test only has to live in fs/nfs to include it.
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

/*
 * pnfs_end_offset(): start + len, saturating at NFS4_MAX_UINT64
 */

struct end_offset_param {
	const char	*desc;
	u64		start;
	u64		len;
	u64		expected;
};

static void end_offset_get_desc(const struct end_offset_param *param,
				char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct end_offset_param end_offset_params[] = {
	{ "empty range at zero",	0, 0, 0 },
	{ "bounded from zero",		0, 100, 100 },
	{ "bounded with offset",	100, 100, 200 },
	{
		.desc		= "unbounded length saturates",
		.start		= 0,
		.len		= NFS4_MAX_UINT64,
		.expected	= NFS4_MAX_UINT64,
	},
	{
		/* start + len would land exactly on the cap */
		.desc		= "sum reaching the cap saturates",
		.start		= NFS4_MAX_UINT64 - 10,
		.len		= 10,
		.expected	= NFS4_MAX_UINT64,
	},
	{
		/* one byte short of the cap still computes normally */
		.desc		= "sum just below the cap is exact",
		.start		= NFS4_MAX_UINT64 - 10,
		.len		= 5,
		.expected	= NFS4_MAX_UINT64 - 5,
	},
	{
		/* would overflow a u64 if it were not saturating */
		.desc		= "overflowing sum saturates",
		.start		= NFS4_MAX_UINT64 - 1,
		.len		= NFS4_MAX_UINT64 - 1,
		.expected	= NFS4_MAX_UINT64,
	},
};

KUNIT_ARRAY_PARAM(end_offset, end_offset_params, end_offset_get_desc);

static void end_offset_case(struct kunit *test)
{
	const struct end_offset_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, pnfs_end_offset(param->start, param->len),
			    param->expected, "start %llu len %llu",
			    param->start, param->len);
}

/* Whatever the inputs, the end can never precede the start. */
static void end_offset_never_precedes_start(struct kunit *test)
{
	const struct end_offset_param *param = test->param_value;

	KUNIT_EXPECT_GE(test, pnfs_end_offset(param->start, param->len),
			param->start);
}

/*
 * pnfs_is_range_intersecting(): half-open [start, end) overlap, with
 * NFS4_MAX_UINT64 meaning "no end"
 */

struct intersect_param {
	const char	*desc;
	u64		start1, end1;
	u64		start2, end2;
	bool		expected;
};

static void intersect_get_desc(const struct intersect_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct intersect_param intersect_params[] = {
	{ "disjoint, second after first",	0, 10, 20, 30, false },
	{ "disjoint, second before first",	20, 30, 0, 10, false },
	{ "partial overlap",			0, 10, 5, 15, true },
	{ "identical ranges",			0, 10, 0, 10, true },
	{ "second contained in first",		0, 100, 40, 50, true },
	{ "first contained in second",		40, 50, 0, 100, true },
	/*
	 * Half-open ranges that merely touch do not intersect: [0,10) and
	 * [10,20) share no byte. This is the off-by-one that matters.
	 */
	{ "touching at the boundary",		0, 10, 10, 20, false },
	{ "overlapping by one byte",		0, 11, 10, 20, true },
	{
		.desc = "unbounded first covers later range",
		.start1 = 0, .end1 = NFS4_MAX_UINT64,
		.start2 = 1000, .end2 = 2000, .expected = true,
	},
	{
		.desc = "unbounded second covers earlier range",
		.start1 = 1000, .end1 = 2000,
		.start2 = 0, .end2 = NFS4_MAX_UINT64, .expected = true,
	},
	{
		.desc = "both unbounded",
		.start1 = 0, .end1 = NFS4_MAX_UINT64,
		.start2 = 5000, .end2 = NFS4_MAX_UINT64, .expected = true,
	},
};

KUNIT_ARRAY_PARAM(intersect, intersect_params, intersect_get_desc);

static void intersect_case(struct kunit *test)
{
	const struct intersect_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test,
			    pnfs_is_range_intersecting(param->start1,
						       param->end1,
						       param->start2,
						       param->end2),
			    param->expected,
			    "[%llu,%llu) vs [%llu,%llu)",
			    param->start1, param->end1,
			    param->start2, param->end2);
}

/* Intersection is symmetric: swapping the operands cannot change it. */
static void intersect_is_symmetric_case(struct kunit *test)
{
	const struct intersect_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test,
			    pnfs_is_range_intersecting(param->start2,
						       param->end2,
						       param->start1,
						       param->end1),
			    param->expected,
			    "swapped operands disagreed");
}

/*
 * pnfs_lseg_range_intersecting(): the same test driven from layout
 * ranges, which are expressed as offset+length rather than start+end
 */

static void lseg_intersecting_uses_offset_and_length(struct kunit *test)
{
	struct pnfs_layout_range a = { .offset = 0,   .length = 10 };
	struct pnfs_layout_range b = { .offset = 5,   .length = 10 };
	struct pnfs_layout_range c = { .offset = 100, .length = 10 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_intersecting(&a, &b));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_intersecting(&a, &c));
}

/*
 * Degenerate zero-length ranges are handled inconsistently, and this
 * pins the actual behaviour rather than the intuitive one.
 *
 * The predicate is `start2 < end1 && start1 < end2`. For an empty range
 * [50,50) that means:
 *
 *   - against itself:      50 < 50 is false, so no intersection
 *   - against [0,100):     0 < 50 and 50 < 100, so it DOES intersect
 *   - against [50,100):    50 < 50 is false, so no intersection
 *
 * So an empty range intersects exactly those ranges that strictly
 * straddle its offset, and never one that merely starts there. Anyone
 * expecting "empty intersects nothing" would be wrong, which is the
 * reason to record it.
 */
static void lseg_zero_length_range_semantics(struct kunit *test)
{
	struct pnfs_layout_range empty = { .offset = 50, .length = 0 };
	struct pnfs_layout_range straddling = { .offset = 0, .length = 100 };
	struct pnfs_layout_range starting_at = { .offset = 50, .length = 50 };

	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_lseg_range_intersecting(&empty, &empty),
			       "an empty range intersected itself");
	KUNIT_EXPECT_TRUE_MSG(test,
			      pnfs_lseg_range_intersecting(&empty, &straddling),
			      "an empty range failed to intersect a range straddling it");
	KUNIT_EXPECT_FALSE_MSG(test,
			       pnfs_lseg_range_intersecting(&empty, &starting_at),
			       "an empty range intersected a range starting at its offset");
}

/*
 * A layout running to end of file is expressed as length
 * NFS4_MAX_UINT64, and must intersect any range at or beyond its offset.
 */
static void lseg_to_end_of_file_intersects_later_ranges(struct kunit *test)
{
	struct pnfs_layout_range eof = {
		.offset = 4096,
		.length = NFS4_MAX_UINT64,
	};
	struct pnfs_layout_range later = { .offset = 1 << 20, .length = 4096 };
	struct pnfs_layout_range earlier = { .offset = 0, .length = 4096 };

	KUNIT_EXPECT_TRUE(test, pnfs_lseg_range_intersecting(&eof, &later));
	KUNIT_EXPECT_FALSE(test, pnfs_lseg_range_intersecting(&eof, &earlier));
}

static struct kunit_case pnfs_end_offset_cases[] = {
	{
		.name			= "end offset",
		.run_case		= end_offset_case,
		.generate_params	= end_offset_gen_params,
	},
	{
		.name			= "end never precedes start",
		.run_case		= end_offset_never_precedes_start,
		.generate_params	= end_offset_gen_params,
	},
	{}
};

static struct kunit_suite pnfs_end_offset_suite = {
	.name		= "pnfs-end-offset",
	.test_cases	= pnfs_end_offset_cases,
};

static struct kunit_case pnfs_intersect_cases[] = {
	{
		.name			= "ranges intersect",
		.run_case		= intersect_case,
		.generate_params	= intersect_gen_params,
	},
	{
		.name			= "intersection is symmetric",
		.run_case		= intersect_is_symmetric_case,
		.generate_params	= intersect_gen_params,
	},
	KUNIT_CASE(lseg_intersecting_uses_offset_and_length),
	KUNIT_CASE(lseg_zero_length_range_semantics),
	KUNIT_CASE(lseg_to_end_of_file_intersects_later_ranges),
	{}
};

static struct kunit_suite pnfs_intersect_suite = {
	.name		= "pnfs-range-intersect",
	.test_cases	= pnfs_intersect_cases,
};

kunit_test_suites(&pnfs_end_offset_suite,
		  &pnfs_intersect_suite);

MODULE_DESCRIPTION("Test pNFS layout range arithmetic");
MODULE_LICENSE("GPL");
