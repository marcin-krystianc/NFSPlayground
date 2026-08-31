// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for NFS page request coalescing in fs/nfs/pagelist.c.
 *
 * Every buffered read or write becomes a struct nfs_page, and the client
 * merges adjacent ones into a single RPC before sending. Whether two
 * requests may merge is decided by nfs_page_is_contiguous(), and how much
 * of the second one fits is decided by the pg_test operation. Get the
 * first wrong and the client either sends corrupt ranges or stops
 * coalescing entirely; get the second wrong and it builds a page array too
 * large for the slab allocator.
 *
 * Both are boundary arithmetic over scalar fields, which is why they can
 * be tested here rather than inferred from I/O behaviour the way xfstests
 * has to. The interesting cases are the ones about folios: a request that
 * starts at page offset 0 may only follow one that ran to the end of its
 * folio, and with large folios "the end" is folio_size(), not PAGE_SIZE.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/nfs.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>

#include "internal.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Private to pagelist.c; un-staticed by scripts/kunit/run-sunrpc-kunit.sh. */
bool nfs_page_is_contiguous(const struct nfs_page *prev,
			    const struct nfs_page *req);
bool nfs_match_lock_context(const struct nfs_lock_context *l1,
			    const struct nfs_lock_context *l2);
unsigned int nfs_coalesce_size(struct nfs_page *prev, struct nfs_page *req,
			       struct nfs_pageio_descriptor *pgio);

/*
 * nfs_page_is_contiguous() reads only scalar fields and compares the page
 * or folio pointers, so a bare struct is enough for most cases. The two
 * branches that call nfs_page_max_length() are the exception: that reads
 * folio_size(), so those tests allocate a real folio.
 */
static struct nfs_page *mkreq(struct kunit *test, pgoff_t index,
			      unsigned int offset, unsigned int pgbase,
			      unsigned int bytes)
{
	struct nfs_page *req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, req);
	req->wb_index = index;
	req->wb_offset = offset;
	req->wb_pgbase = pgbase;
	req->wb_bytes = bytes;
	return req;
}

/* An O_DIRECT-style request, tracking a bare page rather than a folio. */
static void req_set_page(struct nfs_page *req, struct page *page)
{
	clear_bit(PG_FOLIO, &req->wb_flags);
	req->wb_page = page;
}

static void req_set_folio(struct nfs_page *req, struct folio *folio)
{
	set_bit(PG_FOLIO, &req->wb_flags);
	req->wb_folio = folio;
}

static void folio_free_action(void *folio)
{
	folio_put(folio);
}

static struct folio *test_folio(struct kunit *test, unsigned int order)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, order);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, folio);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, folio_free_action,
							folio), 0);
	return folio;
}

/*
 * nfs_page_is_contiguous()
 */

/* The file ranges must abut; a gap ends the coalesce regardless of pages. */
static void gap_in_file_offsets_is_not_contiguous(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 200, 200, 100);
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	req_set_page(prev, page);
	req_set_page(req, page);

	KUNIT_EXPECT_FALSE_MSG(test, nfs_page_is_contiguous(prev, req),
			       "a 100 byte hole did not break contiguity");
	__free_page(page);
}

/*
 * A request starting at page offset 0 begins a new page, so it may only
 * follow one that ran all the way to the end of its own page.
 */
static void page_aligned_follow_on_requires_prev_to_fill_its_page(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, PAGE_SIZE);
	struct nfs_page *req = mkreq(test, 1, 0, 0, 100);
	struct page *a = alloc_page(GFP_KERNEL);
	struct page *b = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	req_set_page(prev, a);
	req_set_page(req, b);

	KUNIT_EXPECT_TRUE(test, nfs_page_is_contiguous(prev, req));
	__free_page(a);
	__free_page(b);
}

/* The same shape, but the previous request stopped mid-page. */
static void page_aligned_follow_on_after_partial_page_is_not_contiguous(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, PAGE_SIZE / 2);
	struct nfs_page *req = mkreq(test, 0, PAGE_SIZE / 2, 0, 100);
	struct page *a = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	req_set_page(prev, a);
	req_set_page(req, a);

	KUNIT_EXPECT_FALSE_MSG(test, nfs_page_is_contiguous(prev, req),
			       "coalesced onto a new page from a half-filled one");
	__free_page(a);
}

/* Continuing within one page: pgbase must pick up exactly where prev ended. */
static void adjacent_within_the_same_page_is_contiguous(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 100, 100, 100);
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	req_set_page(prev, page);
	req_set_page(req, page);

	KUNIT_EXPECT_TRUE(test, nfs_page_is_contiguous(prev, req));
	__free_page(page);
}

/* Same file range, but the data lives in a different page. */
static void adjacent_in_a_different_page_is_not_contiguous(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 100, 100, 100);
	struct page *a = alloc_page(GFP_KERNEL);
	struct page *b = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	req_set_page(prev, a);
	req_set_page(req, b);

	KUNIT_EXPECT_FALSE_MSG(test, nfs_page_is_contiguous(prev, req),
			       "coalesced two requests pointing at different pages");
	__free_page(a);
	__free_page(b);
}

/*
 * File offsets abut but the page offsets do not: pgbase is neither 0 nor
 * the end of prev, so the two describe overlapping or disjoint page data.
 */
static void mismatched_pgbase_is_not_contiguous(struct kunit *test)
{
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 100, 50, 100);
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	req_set_page(prev, page);
	req_set_page(req, page);

	KUNIT_EXPECT_FALSE_MSG(test, nfs_page_is_contiguous(prev, req),
			       "coalesced a request whose pgbase overlapped prev");
	__free_page(page);
}

static void adjacent_within_the_same_folio_is_contiguous(struct kunit *test)
{
	struct folio *folio = test_folio(test, 0);
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 100, 100, 100);

	req_set_folio(prev, folio);
	req_set_folio(req, folio);

	KUNIT_EXPECT_TRUE(test, nfs_page_is_contiguous(prev, req));
}

static void adjacent_in_a_different_folio_is_not_contiguous(struct kunit *test)
{
	struct folio *a = test_folio(test, 0);
	struct folio *b = test_folio(test, 0);
	struct nfs_page *prev = mkreq(test, 0, 0, 0, 100);
	struct nfs_page *req = mkreq(test, 0, 100, 100, 100);

	req_set_folio(prev, a);
	req_set_folio(req, b);

	KUNIT_EXPECT_FALSE_MSG(test, nfs_page_is_contiguous(prev, req),
			       "coalesced two requests pointing at different folios");
}

/*
 * The end of a large folio is folio_size(), not PAGE_SIZE. A request that
 * filled all four pages of an order-2 folio may be followed by a
 * page-aligned one; a request that stopped at the first page may not.
 *
 * Reading PAGE_SIZE here instead of folio_size() would let the client
 * coalesce across a folio boundary it has no data for.
 */
static void large_folio_follow_on_uses_folio_size(struct kunit *test)
{
	struct folio *folio = test_folio(test, 2);
	size_t size = folio_size(folio);
	struct nfs_page *full = mkreq(test, 0, 0, 0, size);
	struct nfs_page *partial = mkreq(test, 0, 0, 0, PAGE_SIZE);
	struct nfs_page *after_full = mkreq(test, size >> PAGE_SHIFT, 0, 0, 100);
	struct nfs_page *after_partial = mkreq(test, 1, 0, 0, 100);

	KUNIT_ASSERT_EQ(test, size, 4 * PAGE_SIZE);
	req_set_folio(full, folio);
	req_set_folio(partial, folio);
	req_set_folio(after_full, folio);
	req_set_folio(after_partial, folio);

	KUNIT_EXPECT_TRUE_MSG(test, nfs_page_is_contiguous(full, after_full),
			      "a request filling the whole folio blocked a follow-on");
	KUNIT_EXPECT_FALSE_MSG(test,
			       nfs_page_is_contiguous(partial, after_partial),
			       "coalesced past the first page of a large folio");
}

/*
 * nfs_generic_pg_test(): how much of a request fits in the current mirror
 */

struct pgio_fixture {
	struct nfs_pageio_descriptor	desc;
	struct nfs_pgio_mirror		mirror;
	struct nfs_pageio_ops		ops;
};

/*
 * pg_get_mirror is left NULL, so nfs_pgio_current_mirror() takes
 * pg_mirrors[0] directly rather than dispatching to a layout driver.
 */
static struct nfs_pageio_descriptor *mkdesc(struct kunit *test, size_t bsize,
					    size_t count)
{
	struct pgio_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);
	f->desc.pg_ops = &f->ops;
	f->desc.pg_mirrors = &f->mirror;
	f->desc.pg_mirror_idx = 0;
	f->mirror.pg_bsize = bsize;
	f->mirror.pg_count = count;
	return &f->desc;
}

static void empty_mirror_takes_the_whole_request(struct kunit *test)
{
	struct nfs_pageio_descriptor *desc = mkdesc(test, 64 * 1024, 0);
	struct nfs_page *req = mkreq(test, 0, 0, 0, PAGE_SIZE);

	KUNIT_EXPECT_EQ(test, nfs_generic_pg_test(desc, NULL, req), PAGE_SIZE);
}

static void full_mirror_takes_nothing(struct kunit *test)
{
	struct nfs_pageio_descriptor *desc = mkdesc(test, PAGE_SIZE, PAGE_SIZE);
	struct nfs_page *req = mkreq(test, 0, 0, 0, PAGE_SIZE);

	KUNIT_EXPECT_EQ(test, nfs_generic_pg_test(desc, NULL, req), 0);
}

/* A partly filled mirror accepts only what is left, not the whole request. */
static void partly_full_mirror_takes_the_remaining_space(struct kunit *test)
{
	struct nfs_pageio_descriptor *desc = mkdesc(test, PAGE_SIZE, 3000);
	struct nfs_page *req = mkreq(test, 0, 0, 0, PAGE_SIZE);

	KUNIT_EXPECT_EQ(test, nfs_generic_pg_test(desc, NULL, req),
			PAGE_SIZE - 3000);
}

static void request_smaller_than_the_space_is_taken_whole(struct kunit *test)
{
	struct nfs_pageio_descriptor *desc = mkdesc(test, 64 * 1024, 0);
	struct nfs_page *req = mkreq(test, 0, 0, 0, 1000);

	KUNIT_EXPECT_EQ(test, nfs_generic_pg_test(desc, NULL, req), 1000);
}

/*
 * The result also has to describe a page array that fits in a single page,
 * independently of pg_bsize. The threshold is derived here rather than
 * written as a constant so the test does not silently drift on an
 * architecture with a different page or pointer size.
 *
 * Not covered: pg_count > pg_bsize, which the function marks as "should
 * never happen" with WARN_ON_ONCE. Provoking it would print a stack trace
 * that kunit.py reports against whichever test is running.
 */
static void oversized_request_is_capped_by_the_page_array_limit(struct kunit *test)
{
	const size_t max_pages = PAGE_SIZE / sizeof(struct page *);
	const size_t fits = max_pages << PAGE_SHIFT;
	const size_t too_big = (max_pages + 1) << PAGE_SHIFT;
	struct nfs_pageio_descriptor *desc = mkdesc(test, too_big * 2, 0);
	struct nfs_page *ok = mkreq(test, 0, 0, 0, fits);
	struct nfs_page *over = mkreq(test, 0, 0, 0, too_big);

	KUNIT_EXPECT_EQ_MSG(test, nfs_generic_pg_test(desc, NULL, ok), fits,
			    "rejected a request whose page array still fits");
	KUNIT_EXPECT_EQ_MSG(test, nfs_generic_pg_test(desc, NULL, over), 0,
			    "accepted a request needing an oversized page array");
}

/*
 * nfs_match_lock_context()
 */

static void identical_lockowners_match(struct kunit *test)
{
	struct nfs_lock_context a = {}, b = {};
	int owner;

	a.lockowner = (fl_owner_t)&owner;
	b.lockowner = (fl_owner_t)&owner;

	KUNIT_EXPECT_TRUE(test, nfs_match_lock_context(&a, &b));
}

static void different_lockowners_do_not_match(struct kunit *test)
{
	struct nfs_lock_context a = {}, b = {};
	int one, two;

	a.lockowner = (fl_owner_t)&one;
	b.lockowner = (fl_owner_t)&two;

	KUNIT_EXPECT_FALSE_MSG(test, nfs_match_lock_context(&a, &b),
			       "two lockowners were treated as one");
}

/*
 * nfs_page_group_sync_on_bit_locked(): the barrier across a page group
 *
 * Requests covering one page form a circular list through wb_this_page,
 * every member pointing at the head. The function sets a bit on one member
 * and reports whether every member now has it -- and when they do, clears
 * the bit across the group so it can be used again.
 */

static struct nfs_page **mkgroup(struct kunit *test, unsigned int n)
{
	struct nfs_page **g;
	unsigned int i;

	g = kunit_kzalloc(test, n * sizeof(*g), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g);

	for (i = 0; i < n; i++) {
		g[i] = kunit_kzalloc(test, sizeof(**g), GFP_KERNEL);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g[i]);
	}
	for (i = 0; i < n; i++) {
		g[i]->wb_this_page = g[(i + 1) % n];
		g[i]->wb_head = g[0];
	}
	/* The function warns if the head lock is not held. */
	set_bit(PG_HEADLOCK, &g[0]->wb_flags);
	return g;
}

static void group_bits_are_clear(struct kunit *test, struct nfs_page **g,
				 unsigned int n, unsigned int bit)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		KUNIT_EXPECT_FALSE_MSG(test, test_bit(bit, &g[i]->wb_flags),
				       "request %u kept the bit after the group completed",
				       i);
}

/* A group of one is complete as soon as its only member arrives. */
static void lone_request_completes_the_group(struct kunit *test)
{
	struct nfs_page **g = mkgroup(test, 1);

	KUNIT_EXPECT_TRUE(test,
			  nfs_page_group_sync_on_bit_locked(g[0], PG_UPTODATE));
	group_bits_are_clear(test, g, 1, PG_UPTODATE);
}

/*
 * The first arrival of two does not complete the group, and its bit stays
 * set -- that is what lets the second arrival see it.
 */
static void first_of_two_does_not_complete_the_group(struct kunit *test)
{
	struct nfs_page **g = mkgroup(test, 2);

	KUNIT_EXPECT_FALSE(test,
			   nfs_page_group_sync_on_bit_locked(g[0], PG_UPTODATE));
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(PG_UPTODATE, &g[0]->wb_flags),
			      "the first arrival did not record itself");
}

static void last_of_two_completes_and_resets_the_group(struct kunit *test)
{
	struct nfs_page **g = mkgroup(test, 2);

	KUNIT_ASSERT_FALSE(test,
			   nfs_page_group_sync_on_bit_locked(g[0], PG_UPTODATE));
	KUNIT_EXPECT_TRUE(test,
			  nfs_page_group_sync_on_bit_locked(g[1], PG_UPTODATE));
	group_bits_are_clear(test, g, 2, PG_UPTODATE);
}

/* Every member has to arrive, not just the head and one other. */
static void every_member_of_a_three_way_group_must_arrive(struct kunit *test)
{
	struct nfs_page **g = mkgroup(test, 3);

	KUNIT_EXPECT_FALSE(test,
			   nfs_page_group_sync_on_bit_locked(g[0], PG_UPTODATE));
	KUNIT_EXPECT_FALSE(test,
			   nfs_page_group_sync_on_bit_locked(g[2], PG_UPTODATE));
	KUNIT_EXPECT_TRUE(test,
			  nfs_page_group_sync_on_bit_locked(g[1], PG_UPTODATE));
	group_bits_are_clear(test, g, 3, PG_UPTODATE);
}

/* Two different bits are tracked independently across the same group. */
static void separate_bits_do_not_interfere(struct kunit *test)
{
	struct nfs_page **g = mkgroup(test, 2);

	KUNIT_ASSERT_FALSE(test,
			   nfs_page_group_sync_on_bit_locked(g[0], PG_UPTODATE));
	KUNIT_EXPECT_FALSE_MSG(test,
			       nfs_page_group_sync_on_bit_locked(g[0], PG_WB_END),
			       "an unrelated bit completed the group");
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(PG_UPTODATE, &g[0]->wb_flags),
			      "syncing one bit cleared another");
}

/*
 * nfs_coalesce_size(): the three gates before a request may be merged
 *
 * Two requests may only share an RPC if they came through the same open
 * (same credential and NFSv4 open state), are not separated by a lock held
 * by a different owner, and describe adjacent data. The contiguity check
 * and the lockowner comparison are covered above; what is tested here is
 * how the three combine, and in particular when the lockowner check is
 * skipped entirely.
 *
 * That last part is the subtle one. Comparing lockowners on a file with no
 * locks would stop unrelated writers from ever coalescing, so the check is
 * gated on the inode actually holding POSIX or flock locks. A fixture that
 * only ever set up the locked case would never notice if that gate were
 * removed.
 */

#define PG_TEST_SENTINEL	0xabcd

/*
 * A stub rather than nfs_generic_pg_test(), so that a 0 result unambiguously
 * means "a gate rejected the pair". The real thing can also return 0, which
 * would make the rejection tests pass for the wrong reason.
 */
static size_t stub_pg_test(struct nfs_pageio_descriptor *desc,
			   struct nfs_page *prev, struct nfs_page *req)
{
	return PG_TEST_SENTINEL;
}

struct coalesce_fixture {
	struct nfs_open_context		ctx_prev, ctx_req;
	struct nfs_lock_context		lock_prev, lock_req;
	struct cred			cred_prev, cred_req;
	struct dentry			dentry;
	struct inode			inode;
	struct file_lock_context	flctx;
	struct nfs_pageio_descriptor	desc;
	struct nfs_pgio_mirror		mirror;
	struct nfs_pageio_ops		ops;
	struct nfs_page			prev, req;
	struct page			*page;
	struct list_head		a_lock;
};

static void coalesce_page_free(void *page)
{
	__free_page(page);
}

/*
 * Builds a pair that coalesces: one open context shared by both requests,
 * adjacent within a single page, and an inode with no lock context at all.
 * Each test then breaks exactly one of those properties.
 */
static struct coalesce_fixture *coalesce_fixture(struct kunit *test)
{
	struct coalesce_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, coalesce_page_free,
							f->page), 0);

	f->dentry.d_inode = &f->inode;
	/* No locks on the file: locks_inode_context() returns NULL. */
	f->inode.i_flctx = NULL;
	INIT_LIST_HEAD(&f->flctx.flc_posix);
	INIT_LIST_HEAD(&f->flctx.flc_flock);
	INIT_LIST_HEAD(&f->flctx.flc_lease);
	INIT_LIST_HEAD(&f->a_lock);

	/*
	 * cred_fscmp() returns 0 immediately for one pointer, so distinct
	 * creds are given matching fsuid/fsgid instead -- that way the
	 * "same open" case still exercises the comparison rather than the
	 * pointer shortcut.
	 */
	f->cred_prev.fsuid = KUIDT_INIT(0);
	f->cred_prev.fsgid = KGIDT_INIT(0);
	f->cred_req = f->cred_prev;

	f->ctx_prev.dentry = &f->dentry;
	f->ctx_prev.cred = &f->cred_prev;
	f->ctx_prev.state = NULL;
	f->ctx_req = f->ctx_prev;
	f->ctx_req.cred = &f->cred_req;

	f->lock_prev.open_context = &f->ctx_prev;
	f->lock_req.open_context = &f->ctx_req;
	f->lock_prev.lockowner = (fl_owner_t)&f->lock_prev;
	f->lock_req.lockowner = f->lock_prev.lockowner;

	/* Adjacent within one page: pgbase picks up where prev ended. */
	f->prev.wb_lock_context = &f->lock_prev;
	f->prev.wb_index = 0;
	f->prev.wb_offset = 0;
	f->prev.wb_pgbase = 0;
	f->prev.wb_bytes = 100;
	f->prev.wb_page = f->page;

	f->req.wb_lock_context = &f->lock_req;
	f->req.wb_index = 0;
	f->req.wb_offset = 100;
	f->req.wb_pgbase = 100;
	f->req.wb_bytes = 100;
	f->req.wb_page = f->page;

	f->ops.pg_test = stub_pg_test;
	f->desc.pg_ops = &f->ops;
	f->desc.pg_mirrors = &f->mirror;
	f->mirror.pg_bsize = 64 * 1024;

	return f;
}

static unsigned int coalesce(struct coalesce_fixture *f)
{
	return nfs_coalesce_size(&f->prev, &f->req, &f->desc);
}

/* The baseline the other tests perturb, and a check that it really passes. */
static void matching_requests_reach_pg_test(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), PG_TEST_SENTINEL,
			    "a pair that should coalesce was rejected");
}

/* The first request of a batch has no predecessor and skips every gate. */
static void absent_prev_skips_the_gates(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	/* Break everything a gate could check; none of it should be read. */
	f->req.wb_pgbase = 4000;
	f->ctx_req.state = (struct nfs4_state *)&f->ctx_req;

	KUNIT_EXPECT_EQ(test, nfs_coalesce_size(NULL, &f->req, &f->desc),
			PG_TEST_SENTINEL);
}

static void different_credential_blocks_coalescing(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->cred_req.fsuid = KUIDT_INIT(1000);

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 0,
			    "coalesced requests from two different users");
}

/* NFSv4: two opens of the same file are still separate open states. */
static void different_open_state_blocks_coalescing(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->ctx_req.state = (struct nfs4_state *)&f->ctx_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 0,
			    "coalesced requests from two different open states");
}

static void non_contiguous_requests_block_coalescing(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	/* Leave a hole between the two ranges. */
	f->req.wb_offset = 500;
	f->req.wb_pgbase = 500;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 0,
			    "coalesced two non-adjacent ranges");
}

/*
 * With no lock context on the inode the lockowners are never compared, so
 * unrelated writers to an unlocked file still share RPCs.
 */
static void unlocked_file_ignores_lockowners(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->lock_req.lockowner = (fl_owner_t)&f->lock_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), PG_TEST_SENTINEL,
			    "compared lockowners on a file with no locks");
}

/* A lock context whose lists are all empty must behave the same way. */
static void empty_lock_lists_ignore_lockowners(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->inode.i_flctx = &f->flctx;
	f->lock_req.lockowner = (fl_owner_t)&f->lock_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), PG_TEST_SENTINEL,
			    "compared lockowners on a file holding no locks");
}

static void posix_lock_makes_lockowner_matter(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->inode.i_flctx = &f->flctx;
	list_add(&f->a_lock, &f->flctx.flc_posix);
	f->lock_req.lockowner = (fl_owner_t)&f->lock_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 0,
			    "coalesced across a POSIX lock held by another owner");
}

static void flock_lock_makes_lockowner_matter(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->inode.i_flctx = &f->flctx;
	list_add(&f->a_lock, &f->flctx.flc_flock);
	f->lock_req.lockowner = (fl_owner_t)&f->lock_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 0,
			    "coalesced across a flock lock held by another owner");
}

/* Locks held by the same owner are no obstacle. */
static void matching_lockowner_coalesces_despite_locks(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->inode.i_flctx = &f->flctx;
	list_add(&f->a_lock, &f->flctx.flc_posix);

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), PG_TEST_SENTINEL,
			    "refused to coalesce under one owner's own lock");
}

/*
 * A lease is not a lock for this purpose: it does not partition writers,
 * so it must not gate coalescing.
 */
static void lease_alone_does_not_make_lockowner_matter(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->inode.i_flctx = &f->flctx;
	list_add(&f->a_lock, &f->flctx.flc_lease);
	f->lock_req.lockowner = (fl_owner_t)&f->lock_req;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), PG_TEST_SENTINEL,
			    "a lease was treated as a lock");
}

/*
 * Having passed the gates, the size comes from pg_test. Driven through the
 * real nfs_generic_pg_test() so the composition is exercised, not just the
 * stub: a mirror with room for less than the request caps the result.
 */
static void size_comes_from_pg_test(struct kunit *test)
{
	struct coalesce_fixture *f = coalesce_fixture(test);

	f->ops.pg_test = nfs_generic_pg_test;
	f->mirror.pg_bsize = PAGE_SIZE;
	f->mirror.pg_count = PAGE_SIZE - 40;

	KUNIT_EXPECT_EQ_MSG(test, coalesce(f), 40,
			    "the coalesced size did not come from pg_test");
}

/*
 * Suites
 */

static struct kunit_case nfs_page_contiguous_cases[] = {
	KUNIT_CASE(gap_in_file_offsets_is_not_contiguous),
	KUNIT_CASE(page_aligned_follow_on_requires_prev_to_fill_its_page),
	KUNIT_CASE(page_aligned_follow_on_after_partial_page_is_not_contiguous),
	KUNIT_CASE(adjacent_within_the_same_page_is_contiguous),
	KUNIT_CASE(adjacent_in_a_different_page_is_not_contiguous),
	KUNIT_CASE(mismatched_pgbase_is_not_contiguous),
	KUNIT_CASE(adjacent_within_the_same_folio_is_contiguous),
	KUNIT_CASE(adjacent_in_a_different_folio_is_not_contiguous),
	KUNIT_CASE(large_folio_follow_on_uses_folio_size),
	{}
};

static struct kunit_suite nfs_page_contiguous_suite = {
	.name		= "nfs-page-contiguous",
	.test_cases	= nfs_page_contiguous_cases,
};

static struct kunit_case nfs_page_pg_test_cases[] = {
	KUNIT_CASE(empty_mirror_takes_the_whole_request),
	KUNIT_CASE(full_mirror_takes_nothing),
	KUNIT_CASE(partly_full_mirror_takes_the_remaining_space),
	KUNIT_CASE(request_smaller_than_the_space_is_taken_whole),
	KUNIT_CASE(oversized_request_is_capped_by_the_page_array_limit),
	{}
};

static struct kunit_suite nfs_page_pg_test_suite = {
	.name		= "nfs-page-pg-test",
	.test_cases	= nfs_page_pg_test_cases,
};

static struct kunit_case nfs_page_lock_context_cases[] = {
	KUNIT_CASE(identical_lockowners_match),
	KUNIT_CASE(different_lockowners_do_not_match),
	{}
};

static struct kunit_suite nfs_page_lock_context_suite = {
	.name		= "nfs-page-lock-context",
	.test_cases	= nfs_page_lock_context_cases,
};

static struct kunit_case nfs_coalesce_cases[] = {
	KUNIT_CASE(matching_requests_reach_pg_test),
	KUNIT_CASE(absent_prev_skips_the_gates),
	KUNIT_CASE(different_credential_blocks_coalescing),
	KUNIT_CASE(different_open_state_blocks_coalescing),
	KUNIT_CASE(non_contiguous_requests_block_coalescing),
	KUNIT_CASE(unlocked_file_ignores_lockowners),
	KUNIT_CASE(empty_lock_lists_ignore_lockowners),
	KUNIT_CASE(posix_lock_makes_lockowner_matter),
	KUNIT_CASE(flock_lock_makes_lockowner_matter),
	KUNIT_CASE(matching_lockowner_coalesces_despite_locks),
	KUNIT_CASE(lease_alone_does_not_make_lockowner_matter),
	KUNIT_CASE(size_comes_from_pg_test),
	{}
};

static struct kunit_suite nfs_coalesce_suite = {
	.name		= "nfs-page-coalesce",
	.test_cases	= nfs_coalesce_cases,
};

static struct kunit_case nfs_page_group_cases[] = {
	KUNIT_CASE(lone_request_completes_the_group),
	KUNIT_CASE(first_of_two_does_not_complete_the_group),
	KUNIT_CASE(last_of_two_completes_and_resets_the_group),
	KUNIT_CASE(every_member_of_a_three_way_group_must_arrive),
	KUNIT_CASE(separate_bits_do_not_interfere),
	{}
};

static struct kunit_suite nfs_page_group_suite = {
	.name		= "nfs-page-group-sync",
	.test_cases	= nfs_page_group_cases,
};

kunit_test_suites(&nfs_page_contiguous_suite,
		  &nfs_page_pg_test_suite,
		  &nfs_page_lock_context_suite,
		  &nfs_page_group_suite,
		  &nfs_coalesce_suite);

MODULE_DESCRIPTION("Test NFS page request coalescing");
MODULE_LICENSE("GPL");
