// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NFSv4.1 session slot table in fs/nfs/nfs4session.c.
 *
 * A session slot table is the client's side of NFSv4.1 exactly-once
 * semantics: every request occupies a numbered slot, and the slot's
 * sequence number lets the server distinguish a retransmission from a new
 * call. Getting slot accounting wrong breaks EOS, so the allocator is
 * worth pinning down.
 *
 * This file exists partly to correct an overstatement made while planning
 * this work: that fs/nfs is not unit-testable because it is entangled with
 * the VFS. That is true of most of it, but not all. The slot table is
 * bitmap allocation plus a control loop, needs no I/O, no VFS and no
 * server, and its whole API is exported through fs/nfs/nfs4session.h.
 */

#include <kunit/test.h>

/* Include order mirrors fs/nfs/nfs4session.c; nfs4_fs.h needs these. */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sunrpc/sched.h>
#include <linux/nfs.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>

#include "nfs4_fs.h"
#include "internal.h"
#include "nfs4session.h"

#define TEST_SLOTS	8

static struct nfs4_slot_table *slot_table_new(struct kunit *test,
					      unsigned int max_reqs)
{
	struct nfs4_slot_table *tbl;

	tbl = kunit_kzalloc(test, sizeof(*tbl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tbl);
	KUNIT_ASSERT_EQ(test,
			nfs4_setup_slot_table(tbl, max_reqs, "kunit-slot-tbl"),
			0);
	return tbl;
}

static void slot_table_free(struct kunit *test, struct nfs4_slot_table *tbl)
{
	nfs4_shutdown_slot_table(tbl);
}

/*
 * A fresh table hands out slot 0 first, and tracks it as the highest in
 * use.
 */
static void alloc_first_slot_is_zero(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slot;

	slot = nfs4_alloc_slot(tbl);
	KUNIT_ASSERT_FALSE(test, IS_ERR(slot));
	KUNIT_EXPECT_EQ(test, slot->slot_nr, 0U);
	KUNIT_EXPECT_EQ(test, tbl->highest_used_slotid, 0U);

	slot_table_free(test, tbl);
}

/* Slots are handed out in ascending order without repeats. */
static void alloc_slots_are_distinct_and_ascending(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slot;
	u32 i;

	for (i = 0; i < TEST_SLOTS; i++) {
		slot = nfs4_alloc_slot(tbl);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(slot),
				       "allocation %u failed", i);
		KUNIT_EXPECT_EQ_MSG(test, slot->slot_nr, i,
				    "expected slot %u", i);
	}

	KUNIT_EXPECT_EQ(test, tbl->highest_used_slotid, TEST_SLOTS - 1);
	slot_table_free(test, tbl);
}

/* Exhausting the table yields -EBUSY rather than an invalid slot. */
static void alloc_past_capacity_returns_ebusy(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slot;
	u32 i;

	for (i = 0; i < TEST_SLOTS; i++) {
		slot = nfs4_alloc_slot(tbl);
		KUNIT_ASSERT_FALSE(test, IS_ERR(slot));
	}

	slot = nfs4_alloc_slot(tbl);
	KUNIT_ASSERT_TRUE(test, IS_ERR(slot));
	KUNIT_EXPECT_EQ(test, PTR_ERR(slot), -EBUSY);

	slot_table_free(test, tbl);
}

/* A freed slot is handed straight back out, lowest free id first. */
static void freed_slot_is_reused(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *first, *second, *again;

	first = nfs4_alloc_slot(tbl);
	KUNIT_ASSERT_FALSE(test, IS_ERR(first));
	second = nfs4_alloc_slot(tbl);
	KUNIT_ASSERT_FALSE(test, IS_ERR(second));

	nfs4_free_slot(tbl, first);

	again = nfs4_alloc_slot(tbl);
	KUNIT_ASSERT_FALSE(test, IS_ERR(again));
	KUNIT_EXPECT_EQ_MSG(test, again->slot_nr, 0U,
			    "allocator did not reuse the lowest free slot");

	slot_table_free(test, tbl);
}

/*
 * highest_used_slotid is reported to the server on every SEQUENCE, so it
 * must fall back to the next-highest slot still in use when the top one is
 * released, not just decrement.
 */
static void freeing_top_slot_recomputes_highest(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slots[4];
	u32 i;

	for (i = 0; i < 4; i++) {
		slots[i] = nfs4_alloc_slot(tbl);
		KUNIT_ASSERT_FALSE(test, IS_ERR(slots[i]));
	}
	KUNIT_ASSERT_EQ(test, tbl->highest_used_slotid, 3U);

	/* Free a middle slot: the high-water mark must not move. */
	nfs4_free_slot(tbl, slots[1]);
	KUNIT_EXPECT_EQ(test, tbl->highest_used_slotid, 3U);

	/* Free the top slot: it must drop to the next one still held. */
	nfs4_free_slot(tbl, slots[3]);
	KUNIT_EXPECT_EQ(test, tbl->highest_used_slotid, 2U);

	slot_table_free(test, tbl);
}

/* Releasing everything marks the table completely idle. */
static void freeing_all_slots_marks_table_empty(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slots[TEST_SLOTS];
	u32 i;

	for (i = 0; i < TEST_SLOTS; i++) {
		slots[i] = nfs4_alloc_slot(tbl);
		KUNIT_ASSERT_FALSE(test, IS_ERR(slots[i]));
	}
	for (i = 0; i < TEST_SLOTS; i++)
		nfs4_free_slot(tbl, slots[i]);

	KUNIT_EXPECT_EQ(test, tbl->highest_used_slotid, (u32)NFS4_NO_SLOT);

	slot_table_free(test, tbl);
}

/* Looking up a slotid beyond the table is refused with -E2BIG. */
static void lookup_beyond_table_is_refused(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slot;

	slot = nfs4_lookup_slot(tbl, tbl->max_slotid + 1);
	KUNIT_ASSERT_TRUE(test, IS_ERR(slot));
	KUNIT_EXPECT_EQ(test, PTR_ERR(slot), -E2BIG);

	slot_table_free(test, tbl);
}

static void lookup_within_table_returns_that_slot(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);
	struct nfs4_slot *slot;

	slot = nfs4_lookup_slot(tbl, 3);
	KUNIT_ASSERT_FALSE(test, IS_ERR(slot));
	KUNIT_EXPECT_EQ(test, slot->slot_nr, 3U);

	slot_table_free(test, tbl);
}

/*
 * The server can shrink or grow the client's usable slot range at runtime
 * via the SEQUENCE reply. Setting a target directly is the non-damped
 * path and must take effect immediately.
 */
static void set_target_slotid_takes_effect(struct kunit *test)
{
	struct nfs4_slot_table *tbl = slot_table_new(test, TEST_SLOTS);

	nfs41_set_target_slotid(tbl, 3);
	KUNIT_EXPECT_EQ(test, tbl->target_highest_slotid, 3U);
	KUNIT_EXPECT_EQ(test, tbl->d_target_highest_slotid, 0);

	slot_table_free(test, tbl);
}

/*
 * Suite
 */

static struct kunit_case nfs4_slot_alloc_cases[] = {
	KUNIT_CASE(alloc_first_slot_is_zero),
	KUNIT_CASE(alloc_slots_are_distinct_and_ascending),
	KUNIT_CASE(alloc_past_capacity_returns_ebusy),
	KUNIT_CASE(freed_slot_is_reused),
	{}
};

static struct kunit_suite nfs4_slot_alloc_suite = {
	.name		= "nfs4-slot-alloc",
	.test_cases	= nfs4_slot_alloc_cases,
};

static struct kunit_case nfs4_slot_accounting_cases[] = {
	KUNIT_CASE(freeing_top_slot_recomputes_highest),
	KUNIT_CASE(freeing_all_slots_marks_table_empty),
	KUNIT_CASE(lookup_beyond_table_is_refused),
	KUNIT_CASE(lookup_within_table_returns_that_slot),
	KUNIT_CASE(set_target_slotid_takes_effect),
	{}
};

static struct kunit_suite nfs4_slot_accounting_suite = {
	.name		= "nfs4-slot-accounting",
	.test_cases	= nfs4_slot_accounting_cases,
};

kunit_test_suites(&nfs4_slot_alloc_suite,
		  &nfs4_slot_accounting_suite);

MODULE_DESCRIPTION("Test NFSv4.1 session slot tables");
MODULE_LICENSE("GPL");
