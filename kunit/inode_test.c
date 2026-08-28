// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for NFS attribute-freshness comparison in fs/nfs/inode.c.
 *
 * nfs_inode_attrs_cmp() decides whether attributes arriving in an RPC
 * reply are newer than what the inode already holds. RPC replies can be
 * reordered, so a stale reply overwriting fresh attributes shows up as
 * cache corruption that is very hard to reproduce deliberately. It is a
 * pure comparison over three inputs and therefore worth pinning down.
 *
 * What this file demonstrates, beyond the tests themselves, is the cost of
 * unit-testing a file like inode.c:
 *
 *   1. The function is file-private, so the runner applies
 *      VISIBLE_IF_KUNIT / EXPORT_SYMBOL_IF_KUNIT to it. That is the
 *      kernel's own mechanism for this, not a workaround.
 *   2. It needs a struct inode, which needs a struct super_block, which
 *      needs a struct nfs_server hanging off s_fs_info. None of those need
 *      to be *real* -- no mount, no VFS registration, no server. Zeroed
 *      allocations with three fields filled in are enough, which is what
 *      nfs_inode_fixture() below builds.
 *
 * The scaffolding is about thirty lines. Functions in inode.c that touch
 * the page cache, take inode locks, or issue RPCs would need far more, and
 * some cannot be reached this way at all.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/nfs.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>

#include "nfs4_fs.h"
#include "internal.h"
#include "iostat.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

int nfs_inode_attrs_cmp(const struct nfs_fattr *fattr,
			const struct inode *inode);
int nfs_attribute_timeout(struct inode *inode);
bool nfs_check_cache_flags_invalid(struct inode *inode, unsigned long flags);
void nfs_ooo_merge(struct nfs_inode *nfsi, u64 start, u64 end);
void nfs_zap_caches_locked(struct inode *inode);
u32 nfs_get_valid_attrmask(struct inode *inode);
bool nfs_file_has_writers(struct nfs_inode *nfsi);
int nfs_update_inode(struct inode *inode, struct nfs_fattr *fattr);
void nfs_wcc_update_inode(struct inode *inode, struct nfs_fattr *fattr);
int nfs_check_inode_attributes(struct inode *inode, struct nfs_fattr *fattr);
int nfs_inode_finish_partial_attr_update(const struct nfs_fattr *fattr,
					 const struct inode *inode);
void nfs_ooo_record(struct nfs_inode *nfsi, struct nfs_fattr *fattr);
void nfs_set_timestamps_to_ts(struct inode *inode, struct iattr *attr);
void nfs_update_timestamps(struct inode *inode, unsigned int ia_valid);
int nfs_find_actor(struct inode *inode, void *opaque);
int nfs_init_locked(struct inode *inode, void *opaque);
bool nfs_getattr_readdirplus_enable(const struct inode *inode);
int __nfs_revalidate_inode(struct nfs_server *server, struct inode *inode);
void nfs_inode_init_regular(struct nfs_inode *nfsi);
void nfs_inode_init_dir(struct nfs_inode *nfsi);
void nfs_init_lock_context(struct nfs_lock_context *l_ctx);
struct nfs_lock_context *__nfs_find_lock_context(struct nfs_open_context *ctx);
void nfs_fattr_fixup_delegated(struct inode *inode, struct nfs_fattr *fattr);
bool nfs_file_has_buffered_writers(struct nfs_inode *nfsi);

/*
 * Mirrors the file-local definition in fs/nfs/inode.c. It is two pointers
 * and has to be constructed here to call the iget5_locked callbacks; if
 * the original ever gains a field this copy must follow.
 */
struct nfs_find_desc {
	struct nfs_fh		*fh;
	struct nfs_fattr	*fattr;
};

/*
 * The minimum viable inode: an nfs_inode (which embeds the vfs inode, so
 * NFS_I() resolves by container_of), a super_block whose s_fs_info points
 * at an nfs_server (so NFS_SERVER() resolves), and nothing else.
 */
struct nfs_inode_fixture {
	struct nfs_inode	nfsi;
	struct super_block	sb;
	struct nfs_server	server;
	struct nfs_client	client;
	struct nfs_rpc_ops	rpc_ops;
	struct address_space	mapping;
};

/*
 * nfs_have_delegated_attributes() reaches the delegation check through
 * NFS_PROTO(inode)->have_delegation, a function pointer. That makes it a
 * seam: a stub returning "no delegation" is enough, with no delegation
 * machinery anywhere in sight.
 */
static int stub_no_delegation(struct inode *inode, fmode_t type, int flags)
{
	return 0;
}

static struct nfs_inode_fixture *nfs_inode_fixture(struct kunit *test,
						   enum nfs4_change_attr_type type,
						   u64 change_attr,
						   unsigned long attr_gencount)
{
	struct nfs_inode_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->rpc_ops.have_delegation = stub_no_delegation;
	f->client.rpc_ops = &f->rpc_ops;
	f->server.nfs_client = &f->client;
	f->server.change_attr_type = type;
	f->sb.s_fs_info = &f->server;

	f->nfsi.vfs_inode.i_sb = &f->sb;
	f->nfsi.vfs_inode.i_mapping = &f->mapping;
	spin_lock_init(&f->nfsi.vfs_inode.i_lock);

	/*
	 * nfs_file_has_writers() calls list_empty() on this, and a zeroed
	 * list_head is not an empty list -- it would read as non-empty and
	 * send the check down the wrong path.
	 */
	INIT_LIST_HEAD(&f->nfsi.open_files);

	f->nfsi.attr_gencount = attr_gencount;
	inode_set_iversion_raw(&f->nfsi.vfs_inode, change_attr);

	return f;
}

/*
 * nfs_zap_caches() bumps a per-cpu statistics counter, so anything
 * reaching it needs real percpu storage on the server. Allocated only by
 * the tests that need it, and released through a KUnit action so it is
 * freed even if an assertion aborts the case.
 */
static void free_iostats(void *stats)
{
	free_percpu((struct nfs_iostats __percpu *)stats);
}

static void fixture_add_iostats(struct kunit *test,
				struct nfs_inode_fixture *f)
{
	f->server.io_stats = alloc_percpu(struct nfs_iostats);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->server.io_stats);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, free_iostats,
						  f->server.io_stats), 0);
}

/* Pretend the mapping holds @n cached pages. Only the count is read. */
static void fixture_set_nrpages(struct nfs_inode_fixture *f, unsigned long n)
{
	f->mapping.nrpages = n;
}

static struct inode *fixture_inode(struct nfs_inode_fixture *f)
{
	return &f->nfsi.vfs_inode;
}

static void init_fattr(struct nfs_fattr *fattr, u64 change_attr,
		       unsigned long gencount)
{
	memset(fattr, 0, sizeof(*fattr));
	fattr->valid = NFS_ATTR_FATTR_CHANGE;
	fattr->change_attr = change_attr;
	fattr->gencount = gencount;
}

/*
 * With a monotonic change attribute, a larger value in the reply means
 * the reply is newer.
 */
static void monotonic_larger_change_attr_is_newer(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 200, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 1);
}

/* An equal change attribute is "no measurable change", not newer. */
static void monotonic_equal_change_attr_is_unchanged(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 100, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 0);
}

/*
 * A smaller change attribute means the reply is stale and must lose, which
 * is the case that protects the attribute cache from reordered replies.
 */
static void monotonic_smaller_change_attr_is_stale(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  200, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 100, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)),
			-1);
}

/*
 * Under strict monotonic semantics there is no "unchanged" verdict: equal
 * counts as stale, unlike the merely-monotonic case above. Both are driven
 * from the same table by change_attr_type, so this pins the difference.
 */
static void strict_monotonic_equal_change_attr_is_stale(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_MONOTONIC_INCR,
				  100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 100, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)),
			-1);
}

static void strict_monotonic_larger_change_attr_is_newer(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_MONOTONIC_INCR,
				  100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 101, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 1);
}

/*
 * When the server gives no usable change attribute the comparison has
 * nothing to go on and must answer "not sure" rather than guess.
 */
static void undefined_change_type_is_undecided(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 999, 0);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 0);
}

/*
 * Even under a monotonic server, a reply that carries no change attribute
 * at all cannot be compared on that basis.
 */
static void missing_change_attr_is_undecided(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 999, 0);
	fattr.valid = 0;	/* no NFS_ATTR_FATTR_CHANGE */

	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 0);
}

/*
 * The generic check runs first and wins outright: a reply whose gencount
 * is ahead of the inode's is treated as newer regardless of what the
 * change attribute says. Here the change attribute alone would say
 * "stale".
 */
static void newer_gencount_overrides_change_attr(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  200, 5);
	struct nfs_fattr fattr;

	init_fattr(&fattr, 100, 6);
	KUNIT_EXPECT_EQ(test, nfs_inode_attrs_cmp(&fattr, fixture_inode(f)), 1);
}

/*
 * Cache invalidation: nfs_zap_mapping() and nfs_set_cache_invalid()
 *
 * These look like they need a working page cache, and they do not. Both
 * only read mapping->nrpages as a count, so a zeroed address_space with
 * that one field set is sufficient. The lock is a real spinlock, taken and
 * released with nothing contending it.
 */

/*
 * There is no data cache to invalidate when no pages are cached, so
 * nfs_zap_mapping() must do nothing at all rather than set the flag.
 */
static void zap_mapping_without_pages_is_a_noop(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_set_nrpages(f, 0);
	f->nfsi.cache_validity = 0;

	nfs_zap_mapping(inode, inode->i_mapping);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			0UL);
}

static void zap_mapping_with_pages_invalidates_data(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_set_nrpages(f, 4);
	f->nfsi.cache_validity = 0;

	nfs_zap_mapping(inode, inode->i_mapping);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
}

/*
 * The same rule is enforced a second time inside nfs_set_cache_invalid():
 * asking for NFS_INO_INVALID_DATA on a mapping with no pages has the flag
 * stripped rather than stored.
 */
static void set_cache_invalid_strips_data_flag_without_pages(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_set_nrpages(f, 0);
	f->nfsi.cache_validity = 0;

	spin_lock(&inode->i_lock);
	nfs_set_cache_invalid(inode, NFS_INO_INVALID_DATA);
	spin_unlock(&inode->i_lock);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			0UL);
}

/* Flags accumulate: a new request never clears validity bits already set. */
static void set_cache_invalid_accumulates_flags(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = NFS_INO_INVALID_ACCESS;

	spin_lock(&inode->i_lock);
	nfs_set_cache_invalid(inode, NFS_INO_INVALID_MODE);
	spin_unlock(&inode->i_lock);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_MODE,
			(unsigned long)NFS_INO_INVALID_MODE);
}

/*
 * NFS_INO_REVAL_FORCED is an instruction to this call, not a state to
 * remember, so it must never survive into cache_validity.
 */
static void set_cache_invalid_does_not_store_reval_forced(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = 0;

	spin_lock(&inode->i_lock);
	nfs_set_cache_invalid(inode,
			      NFS_INO_INVALID_ATTR | NFS_INO_REVAL_FORCED);
	spin_unlock(&inode->i_lock);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_REVAL_FORCED,
			0UL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATTR,
			(unsigned long)NFS_INO_INVALID_ATTR);
}

/*
 * Attribute cache expiry
 *
 * nfs_attribute_timeout() is jiffies arithmetic over a window that opens
 * at read_cache_jiffies and lasts attrtimeo.
 */

static void attribute_timeout_within_window_is_fresh(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;

	KUNIT_EXPECT_EQ(test, nfs_attribute_timeout(fixture_inode(f)), 0);
}

static void attribute_timeout_past_window_has_expired(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	/* Window opened two minutes ago and lasted one second. */
	f->nfsi.read_cache_jiffies = jiffies - 120 * HZ;
	f->nfsi.attrtimeo = HZ;

	KUNIT_EXPECT_EQ(test, nfs_attribute_timeout(fixture_inode(f)), 1);
}

/* A zero timeout means the cache is never considered fresh. */
static void attribute_timeout_zero_expires_immediately(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 0;

	KUNIT_EXPECT_EQ(test, nfs_attribute_timeout(fixture_inode(f)), 1);
}

/*
 * A delegation means the client owns the attributes, so expiry does not
 * apply however old the cache is. The stub in the fixture is swapped for
 * one claiming a delegation.
 */
static int stub_has_delegation(struct inode *inode, fmode_t type, int flags)
{
	return 1;
}

static void delegated_attributes_never_expire(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.read_cache_jiffies = jiffies - 120 * HZ;
	f->nfsi.attrtimeo = HZ;

	KUNIT_EXPECT_EQ(test, nfs_attribute_cache_expired(fixture_inode(f)), 1);

	f->rpc_ops.have_delegation = stub_has_delegation;
	KUNIT_EXPECT_EQ(test, nfs_attribute_cache_expired(fixture_inode(f)), 0);
}

/* An explicitly flagged invalidity is reported without consulting time. */
static void check_cache_invalid_honours_flags(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;
	f->nfsi.cache_validity = NFS_INO_INVALID_ACCESS;

	KUNIT_EXPECT_TRUE(test,
			  nfs_check_cache_invalid(fixture_inode(f),
						  NFS_INO_INVALID_ACCESS));
	KUNIT_EXPECT_FALSE(test,
			   nfs_check_cache_invalid(fixture_inode(f),
						   NFS_INO_INVALID_MODE));
}

static void check_cache_flags_invalid_matches_any_bit(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.cache_validity = NFS_INO_INVALID_MODE;

	KUNIT_EXPECT_TRUE(test,
			  nfs_check_cache_flags_invalid(fixture_inode(f),
					NFS_INO_INVALID_MODE |
					NFS_INO_INVALID_ACCESS));
	KUNIT_EXPECT_FALSE(test,
			   nfs_check_cache_flags_invalid(fixture_inode(f),
					NFS_INO_INVALID_ACCESS));
}

/*
 * Out-of-order reply tracking
 *
 * When change attributes arrive out of order the client records the gap
 * so it can tell later whether the data cache is trustworthy. Gaps merge
 * when they abut, and the table is small enough that overflow has to be
 * handled by giving up and setting NFS_INO_DATA_INVAL_DEFER.
 */

static void ooo_records_a_single_gap(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	nfs_ooo_merge(&f->nfsi, 10, 20);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->cnt, 1);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].start, 10ULL);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].end, 20ULL);
	kfree(f->nfsi.ooo);
}

/* A gap ending where an existing one starts is merged, not appended. */
static void ooo_merges_abutting_gap_below(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	nfs_ooo_merge(&f->nfsi, 10, 20);
	nfs_ooo_merge(&f->nfsi, 5, 10);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.ooo->cnt, 1,
			    "abutting gaps were not merged");
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].start, 5ULL);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].end, 20ULL);
	kfree(f->nfsi.ooo);
}

static void ooo_merges_abutting_gap_above(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	nfs_ooo_merge(&f->nfsi, 10, 20);
	nfs_ooo_merge(&f->nfsi, 20, 30);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->cnt, 1);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].start, 10ULL);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->gap[0].end, 30ULL);
	kfree(f->nfsi.ooo);
}

/* Non-adjacent gaps are kept separately. */
static void ooo_keeps_disjoint_gaps_apart(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	nfs_ooo_merge(&f->nfsi, 10, 20);
	nfs_ooo_merge(&f->nfsi, 100, 200);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->cnt, 2);
	kfree(f->nfsi.ooo);
}

/* An empty range collapses to nothing rather than being recorded. */
static void ooo_empty_range_records_nothing(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	nfs_ooo_merge(&f->nfsi, 42, 42);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->cnt, 0);
	kfree(f->nfsi.ooo);
}

/*
 * The gap table is fixed size. Overflowing it must fall back to the
 * blunt NFS_INO_DATA_INVAL_DEFER flag and release the table, rather than
 * writing past the end of the array.
 */
static void ooo_overflow_falls_back_to_defer(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	u64 base;
	int i;

	/* Disjoint gaps, spaced so none of them can merge. */
	for (i = 0; i < 64; i++) {
		base = 100ULL * (i + 1);
		nfs_ooo_merge(&f->nfsi, base, base + 10);
		if (f->nfsi.cache_validity & NFS_INO_DATA_INVAL_DEFER)
			break;
	}

	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_DATA_INVAL_DEFER,
			    (unsigned long)NFS_INO_DATA_INVAL_DEFER,
			    "gap table overflow did not fall back to defer");
	KUNIT_EXPECT_PTR_EQ_MSG(test, f->nfsi.ooo, NULL,
				"gap table was not released on overflow");
}

/* Once deferred, there is nothing to gain from recording more gaps. */
static void ooo_merge_is_skipped_once_deferred(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->nfsi.cache_validity = NFS_INO_DATA_INVAL_DEFER;
	nfs_ooo_merge(&f->nfsi, 10, 20);

	KUNIT_EXPECT_PTR_EQ(test, f->nfsi.ooo, NULL);
}

/*
 * Wholesale cache invalidation
 */

/*
 * A regular file has data worth invalidating; a device node or fifo does
 * not, so nfs_zap_caches_locked() withholds NFS_INO_INVALID_DATA for it.
 */
static void zap_caches_invalidates_data_for_regular_files(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_add_iostats(test, f);
	fixture_set_nrpages(f, 1);
	inode->i_mode = S_IFREG | 0644;

	nfs_zap_caches(inode);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
}

static void zap_caches_withholds_data_for_special_files(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_add_iostats(test, f);
	fixture_set_nrpages(f, 1);
	inode->i_mode = S_IFCHR | 0644;

	nfs_zap_caches(inode);

	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_DATA, 0UL,
			    "data cache invalidated for a character device");
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATTR,
			(unsigned long)NFS_INO_INVALID_ATTR);
}

/* Invalidating atime touches only the atime bit. */
static void invalidate_atime_sets_only_atime(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = 0;

	nfs_invalidate_atime(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATIME,
			(unsigned long)NFS_INO_INVALID_ATIME);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			0UL);
}

/*
 * Small helpers
 */

/*
 * A 64-bit NFS fileid has to be folded into ino_t, which on 32-bit is
 * narrower. The fold XORs the high half in rather than truncating, so
 * two fileids differing only above 32 bits still land on different
 * inode numbers.
 */
static void fileid_to_ino_folds_high_bits(struct kunit *test)
{
	u64 a = 0x1122334400000000ULL;
	u64 b = 0x8877665500000000ULL;

	KUNIT_EXPECT_NE_MSG(test, nfs_fileid_to_ino_t(a),
			    nfs_fileid_to_ino_t(b),
			    "fileids differing only in the high word collided");
	/* A fileid that already fits is passed through unchanged. */
	KUNIT_EXPECT_EQ(test, nfs_fileid_to_ino_t(0x1234ULL), (ino_t)0x1234);
}

/*
 * nfs_get_valid_attrmask() turns "which parts of the cache are invalid"
 * into "which statx fields we can answer without asking the server". Type
 * and inode number are always answerable.
 */
static void valid_attrmask_reports_everything_when_cache_is_good(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	u32 mask;

	f->nfsi.cache_validity = 0;
	mask = nfs_get_valid_attrmask(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, mask & STATX_INO, (u32)STATX_INO);
	KUNIT_EXPECT_EQ(test, mask & STATX_TYPE, (u32)STATX_TYPE);
	KUNIT_EXPECT_EQ(test, mask & STATX_SIZE, (u32)STATX_SIZE);
	KUNIT_EXPECT_EQ(test, mask & STATX_MODE, (u32)STATX_MODE);
	KUNIT_EXPECT_EQ(test, mask & STATX_MTIME, (u32)STATX_MTIME);
}

static void valid_attrmask_drops_invalidated_fields(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	u32 mask;

	f->nfsi.cache_validity = NFS_INO_INVALID_SIZE | NFS_INO_INVALID_MODE;
	mask = nfs_get_valid_attrmask(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, mask & STATX_SIZE, 0U);
	KUNIT_EXPECT_EQ(test, mask & STATX_MODE, 0U);
	/* Unrelated fields survive, and type/ino are always reported. */
	KUNIT_EXPECT_EQ(test, mask & STATX_MTIME, (u32)STATX_MTIME);
	KUNIT_EXPECT_EQ(test, mask & STATX_INO, (u32)STATX_INO);
}

/* UID and GID move together, both gated on NFS_INO_INVALID_OTHER. */
static void valid_attrmask_pairs_uid_and_gid(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	u32 mask;

	f->nfsi.cache_validity = NFS_INO_INVALID_OTHER;
	mask = nfs_get_valid_attrmask(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, mask & STATX_UID, 0U);
	KUNIT_EXPECT_EQ(test, mask & STATX_GID, 0U);
}

/* Only regular files can have writers, whatever the open list says. */
static void file_has_writers_is_false_for_non_regular(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_inode(f)->i_mode = S_IFDIR | 0755;
	KUNIT_EXPECT_FALSE(test, nfs_file_has_writers(&f->nfsi));
}

static void file_has_writers_is_false_without_open_files(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	KUNIT_EXPECT_FALSE(test, nfs_file_has_writers(&f->nfsi));
}

/*
 * nfs_zap_acl_cache() dispatches through NFS_PROTO()->clear_acl_cache,
 * another function-pointer seam, and then clears the ACL validity bit.
 */
static int acl_cache_cleared;

static void stub_clear_acl_cache(struct inode *inode)
{
	acl_cache_cleared++;
}

static void zap_acl_cache_calls_protocol_hook_and_clears_flag(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	acl_cache_cleared = 0;
	f->rpc_ops.clear_acl_cache = stub_clear_acl_cache;
	f->nfsi.cache_validity = NFS_INO_INVALID_ACL | NFS_INO_INVALID_ACCESS;

	nfs_zap_acl_cache(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, acl_cache_cleared, 1);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACL,
			0UL);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			    (unsigned long)NFS_INO_INVALID_ACCESS,
			    "unrelated validity bit was cleared");
}

/* A protocol with no ACL support simply has no hook to call. */
static void zap_acl_cache_tolerates_absent_hook(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	f->rpc_ops.clear_acl_cache = NULL;
	f->nfsi.cache_validity = NFS_INO_INVALID_ACL;

	nfs_zap_acl_cache(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACL,
			0UL);
}

/* Marking an inode stale sets the flag and zaps the caches with it. */
static void set_inode_stale_flags_and_zaps(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);

	fixture_add_iostats(test, f);
	fixture_set_nrpages(f, 1);
	inode->i_mode = S_IFREG | 0644;

	nfs_set_inode_stale(inode);

	KUNIT_EXPECT_TRUE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATTR,
			(unsigned long)NFS_INO_INVALID_ATTR);
}

/*
 * Allocation helpers
 */

static void alloc_fattr_starts_invalid(struct kunit *test)
{
	struct nfs_fattr *fattr = nfs_alloc_fattr();

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fattr);
	KUNIT_EXPECT_EQ_MSG(test, fattr->valid, 0U,
			    "a fresh fattr claimed to hold valid attributes");
	KUNIT_EXPECT_PTR_EQ(test, fattr->label, NULL);
	nfs_free_fattr(fattr);
}

/*
 * The generation counter is what orders concurrent replies, so two
 * successive initialisations must never produce the same value.
 */
static void fattr_init_advances_generation_counter(struct kunit *test)
{
	struct nfs_fattr first, second;

	nfs_fattr_init(&first);
	nfs_fattr_init(&second);

	KUNIT_EXPECT_NE_MSG(test, first.gencount, second.gencount,
			    "attribute generation counter did not advance");
}

static void fattr_set_barrier_advances_generation(struct kunit *test)
{
	struct nfs_fattr fattr;
	unsigned long before;

	nfs_fattr_init(&fattr);
	before = fattr.gencount;
	nfs_fattr_set_barrier(&fattr);

	KUNIT_EXPECT_NE(test, fattr.gencount, before);
}

static void alloc_fhandle_starts_empty(struct kunit *test)
{
	struct nfs_fh *fh = nfs_alloc_fhandle();

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fh);
	KUNIT_EXPECT_EQ(test, fh->size, 0U);
	nfs_free_fhandle(fh);
}

/*
 * nfs_update_inode(): the guards
 *
 * This is the function nfs_inode_attrs_cmp() exists to protect, and its
 * first job is refusing to apply attributes that plainly belong to a
 * different object. Getting that wrong means silently adopting another
 * file's identity, so the rejection paths are the ones worth pinning.
 *
 * It must be called with i_lock held.
 */

static int call_update_inode(struct nfs_inode_fixture *f,
			     struct nfs_fattr *fattr)
{
	struct inode *inode = fixture_inode(f);
	int ret;

	spin_lock(&inode->i_lock);
	ret = nfs_update_inode(inode, fattr);
	spin_unlock(&inode->i_lock);
	return ret;
}

/* A reply for a different fileid is refused and marks the inode stale. */
static void update_inode_rejects_changed_fileid(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_add_iostats(test, f);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_FILEID;
	fattr.fileid = 2000;

	KUNIT_EXPECT_EQ_MSG(test, call_update_inode(f, &fattr), -ESTALE,
			    "attributes for a different fileid were accepted");
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
}

/*
 * Unless the mismatch is explained by the mounted-on fileid, which is the
 * legitimate case at a mountpoint crossing.
 */
static void update_inode_allows_mounted_on_fileid(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_MOUNTED_ON_FILEID;
	fattr.fileid = 2000;
	fattr.mounted_on_fileid = 1000;

	KUNIT_EXPECT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
}

/* A reply carrying only a mounted-on fileid is ignored, not applied. */
static void update_inode_ignores_mounted_on_fileid_only(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_MOUNTED_ON_FILEID;
	fattr.mounted_on_fileid = 1000;

	KUNIT_EXPECT_EQ(test, call_update_inode(f, &fattr), 0);
}

/*
 * A file that has become a directory on the server is not the same
 * object, so the attributes must be refused rather than applied over the
 * top of the wrong inode type.
 */
static void update_inode_rejects_changed_type(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_add_iostats(test, f);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_TYPE;
	fattr.fileid = 1000;
	fattr.mode = S_IFDIR | 0755;

	KUNIT_EXPECT_EQ_MSG(test, call_update_inode(f, &fattr), -ESTALE,
			    "a regular file accepted directory attributes");
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
}

/* The matching case is accepted and refreshes the revalidation timestamp. */
static void update_inode_accepts_matching_identity(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;
	f->nfsi.read_cache_jiffies = 0;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_TYPE;
	fattr.fileid = 1000;
	fattr.mode = S_IFREG | 0644;
	fattr.time_start = jiffies;

	KUNIT_EXPECT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.read_cache_jiffies, fattr.time_start,
			    "revalidation timestamp was not refreshed");
}

/*
 * Weak cache consistency
 *
 * NFSv3 and later can return an attribute both before and after an
 * operation. If the "before" value matches what the client already holds,
 * nothing else changed the file in between, so the "after" value can be
 * adopted without a fresh GETATTR. If it does not match, someone else
 * touched the file and the update must be discarded -- applying it anyway
 * would silently overwrite another client's change.
 *
 * Each attribute is gated independently, so the tests below check that a
 * mismatch on one does not leak into another.
 */

static void wcc_applies_change_attr_when_pre_matches(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRECHANGE | NFS_ATTR_FATTR_CHANGE;
	fattr.pre_change_attr = 100;	/* matches the inode */
	fattr.change_attr = 200;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, inode_peek_iversion_raw(inode), 200ULL);
}

/* The critical negative: a stale "before" value means do not apply. */
static void wcc_discards_change_attr_when_pre_differs(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRECHANGE | NFS_ATTR_FATTR_CHANGE;
	fattr.pre_change_attr = 999;	/* the file moved under us */
	fattr.change_attr = 200;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ_MSG(test, inode_peek_iversion_raw(inode), 100ULL,
			    "a non-atomic change attribute was applied");
}

/* Without the "before" half there is no atomicity guarantee to rely on. */
static void wcc_ignores_change_attr_without_prechange(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE;
	fattr.change_attr = 200;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, inode_peek_iversion_raw(inode), 100ULL);
}

/*
 * An atomic change to a directory invalidates its cached contents, since
 * the change attribute moving means entries were added or removed.
 */
static void wcc_change_on_directory_invalidates_data(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	inode->i_mode = S_IFDIR | 0755;
	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = 0;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRECHANGE | NFS_ATTR_FATTR_CHANGE;
	fattr.pre_change_attr = 100;
	fattr.change_attr = 200;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
}

static void wcc_applies_mtime_when_pre_matches(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct timespec64 before = { .tv_sec = 1000, .tv_nsec = 0 };
	struct timespec64 after = { .tv_sec = 2000, .tv_nsec = 0 };
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	inode_set_mtime_to_ts(inode, before);

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PREMTIME | NFS_ATTR_FATTR_MTIME;
	fattr.pre_mtime = before;
	fattr.mtime = after;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, inode_get_mtime_sec(inode), 2000LL);
}

static void wcc_discards_mtime_when_pre_differs(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct timespec64 held = { .tv_sec = 1000, .tv_nsec = 0 };
	struct timespec64 stale = { .tv_sec = 500, .tv_nsec = 0 };
	struct timespec64 after = { .tv_sec = 2000, .tv_nsec = 0 };
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	inode_set_mtime_to_ts(inode, held);

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PREMTIME | NFS_ATTR_FATTR_MTIME;
	fattr.pre_mtime = stale;
	fattr.mtime = after;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ_MSG(test, inode_get_mtime_sec(inode), 1000LL,
			    "a non-atomic mtime was applied");
}

static void wcc_applies_ctime_when_pre_matches(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct timespec64 before = { .tv_sec = 1000, .tv_nsec = 0 };
	struct timespec64 after = { .tv_sec = 2000, .tv_nsec = 0 };
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	inode_set_ctime_to_ts(inode, before);

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRECTIME | NFS_ATTR_FATTR_CTIME;
	fattr.pre_ctime = before;
	fattr.ctime = after;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, inode_get_ctime_sec(inode), 2000LL);
}

static void wcc_applies_size_when_pre_matches(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	i_size_write(inode, 4096);

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRESIZE | NFS_ATTR_FATTR_SIZE;
	fattr.pre_size = 4096;
	fattr.size = 8192;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ(test, i_size_read(inode), 8192LL);
}

/*
 * With writes still in flight the client's own size is authoritative, so
 * a server-supplied size must not overwrite it even when the pre-value
 * matches.
 */
static void wcc_discards_size_while_writebacks_pending(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	i_size_write(inode, 4096);
	atomic_long_set(&f->nfsi.nrequests, 1);

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_PRESIZE | NFS_ATTR_FATTR_SIZE;
	fattr.pre_size = 4096;
	fattr.size = 8192;

	nfs_wcc_update_inode(inode, &fattr);

	KUNIT_EXPECT_EQ_MSG(test, i_size_read(inode), 4096LL,
			    "server size overwrote a locally extended file");
}

/*
 * nfs_refresh_inode(): the dispatcher
 */

/* A reply carrying no attributes at all is a no-op, not an error. */
static void refresh_inode_ignores_empty_fattr(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = 0;

	KUNIT_EXPECT_EQ(test, nfs_refresh_inode(fixture_inode(f), &fattr), 0);
}

/*
 * A newer reply is routed into nfs_update_inode(), so a fileid mismatch
 * is still caught here rather than slipping through the dispatcher.
 */
static void refresh_inode_propagates_identity_rejection(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	fixture_add_iostats(test, f);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_CHANGE;
	fattr.fileid = 2000;
	fattr.change_attr = 500;	/* newer, so it will be applied */

	KUNIT_EXPECT_EQ(test, nfs_refresh_inode(fixture_inode(f), &fattr),
			-ESTALE);
}

/*
 * nfs_check_inode_attributes()
 *
 * Called when a reply is neither newer nor older than what the inode
 * holds. It cannot apply anything, so instead it compares each attribute
 * and flags the corresponding cache bit wherever the server disagrees
 * with the client. A missed comparison means serving stale data; an
 * over-eager one means needless GETATTRs.
 *
 * Every case below starts from an inode and a fattr that agree, then
 * perturbs exactly one attribute, so a flag appearing anywhere else is a
 * leak between comparisons.
 */

#define CHECK_MODE	(S_IFREG | 0644)
#define CHECK_SIZE	4096
#define CHECK_NLINK	1

static struct nfs_inode_fixture *check_attrs_fixture(struct kunit *test,
						     struct nfs_fattr *fattr)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct timespec64 ts = { .tv_sec = 1000, .tv_nsec = 0 };

	inode->i_mode = CHECK_MODE;
	set_nlink(inode, CHECK_NLINK);
	i_size_write(inode, CHECK_SIZE);
	inode_set_mtime_to_ts(inode, ts);
	inode_set_ctime_to_ts(inode, ts);
	inode_set_atime_to_ts(inode, ts);
	f->nfsi.fileid = 1000;
	f->nfsi.cache_validity = 0;

	/* A fattr that agrees with the inode in every respect. */
	memset(fattr, 0, sizeof(*fattr));
	fattr->valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_TYPE |
		       NFS_ATTR_FATTR_CHANGE | NFS_ATTR_FATTR_MTIME |
		       NFS_ATTR_FATTR_CTIME | NFS_ATTR_FATTR_SIZE |
		       NFS_ATTR_FATTR_MODE | NFS_ATTR_FATTR_NLINK |
		       NFS_ATTR_FATTR_ATIME;
	fattr->fileid = 1000;
	fattr->mode = CHECK_MODE;
	fattr->change_attr = 100;
	fattr->mtime = ts;
	fattr->ctime = ts;
	fattr->atime = ts;
	fattr->size = CHECK_SIZE;
	fattr->nlink = CHECK_NLINK;

	return f;
}

static int call_check_attrs(struct nfs_inode_fixture *f,
			    struct nfs_fattr *fattr)
{
	struct inode *inode = fixture_inode(f);
	int ret;

	spin_lock(&inode->i_lock);
	ret = nfs_check_inode_attributes(inode, fattr);
	spin_unlock(&inode->i_lock);
	return ret;
}

/* Full agreement must leave the cache entirely valid. */
static void check_attrs_agreeing_flags_nothing(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_validity, 0UL,
			    "attributes agreed but the cache was invalidated");
}

static void check_attrs_flags_changed_size(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.size = CHECK_SIZE * 2;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_SIZE,
			(unsigned long)NFS_INO_INVALID_SIZE);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_MODE, 0UL,
			    "a size mismatch leaked into the mode flag");
}

static void check_attrs_flags_changed_mtime(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.mtime.tv_sec = 2000;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_MTIME,
			(unsigned long)NFS_INO_INVALID_MTIME);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_CTIME,
			0UL);
}

static void check_attrs_flags_changed_change_attr(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.change_attr = 999;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_CHANGE,
			(unsigned long)NFS_INO_INVALID_CHANGE);
}

/*
 * Only the permission bits are compared, so a mode differing solely in
 * the file-type bits must not be reported as a permission change. (A real
 * type change is caught earlier, as -ESTALE.)
 */
static void check_attrs_compares_only_permission_bits(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.valid &= ~NFS_ATTR_FATTR_TYPE;
	fattr.mode = 0644;	/* same permissions, no type bits */

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_MODE, 0UL,
			    "file-type bits were treated as a permission change");
}

static void check_attrs_flags_changed_permissions(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.mode = S_IFREG | 0600;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_MODE,
			(unsigned long)NFS_INO_INVALID_MODE);
}

/* Owner and group share a single validity bit. */
static void check_attrs_maps_owner_change_to_other(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.valid |= NFS_ATTR_FATTR_OWNER;
	fattr.uid = KUIDT_INIT(4242);

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_OTHER,
			(unsigned long)NFS_INO_INVALID_OTHER);
}

static void check_attrs_flags_changed_nlink(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.nlink = CHECK_NLINK + 1;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_NLINK,
			(unsigned long)NFS_INO_INVALID_NLINK);
}

static void check_attrs_flags_changed_atime(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.atime.tv_sec = 2000;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATIME,
			(unsigned long)NFS_INO_INVALID_ATIME);
}

/* The identity guards are enforced here too, not only in update_inode. */
static void check_attrs_rejects_changed_fileid(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.fileid = 2000;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), -ESTALE);
}

static void check_attrs_rejects_changed_type(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.mode = S_IFDIR | 0755;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), -ESTALE);
}

/*
 * Holding a delegation means the client owns these attributes, so there
 * is nothing to compare against and nothing to invalidate -- even when
 * the server's values differ wildly.
 */
static void check_attrs_skips_everything_when_delegated(struct kunit *test)
{
	struct nfs_fattr fattr;
	struct nfs_inode_fixture *f = check_attrs_fixture(test, &fattr);

	fattr.size = CHECK_SIZE * 4;
	fattr.mode = S_IFREG | 0600;
	f->rpc_ops.have_delegation = stub_has_delegation;

	KUNIT_EXPECT_EQ(test, call_check_attrs(f, &fattr), 0);
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_validity, 0UL,
			    "delegated attributes were invalidated");
}

/*
 * nfs_post_op_update_inode()
 */

/*
 * A directory's contents are invalidated on any post-op update, because
 * the operation that just completed may have added or removed entries.
 */
static void post_op_update_invalidates_directory_data(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	inode->i_mode = S_IFDIR | 0755;
	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = 0;

	nfs_fattr_init(&fattr);

	KUNIT_EXPECT_EQ(test, nfs_post_op_update_inode(inode, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
}

/* A regular file gets the change and ctime bits, but not data. */
static void post_op_update_leaves_regular_file_data_alone(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;

	inode->i_mode = S_IFREG | 0644;
	fixture_set_nrpages(f, 1);
	f->nfsi.cache_validity = 0;

	nfs_fattr_init(&fattr);

	KUNIT_EXPECT_EQ(test, nfs_post_op_update_inode(inode, &fattr), 0);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_CHANGE,
			(unsigned long)NFS_INO_INVALID_CHANGE);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_DATA, 0UL,
			    "a post-op update invalidated file data");
}

/* The barrier is bumped so racing older replies cannot win afterwards. */
static void post_op_update_sets_a_barrier(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;
	unsigned long before;

	fixture_inode(f)->i_mode = S_IFREG | 0644;
	nfs_fattr_init(&fattr);
	before = fattr.gencount;

	nfs_post_op_update_inode(fixture_inode(f), &fattr);

	KUNIT_EXPECT_NE_MSG(test, fattr.gencount, before,
			    "post-op update did not set an attribute barrier");
}

/*
 * nfs_update_inode(): the body
 *
 * Past the identity guards, this is where server attributes are actually
 * written into the inode. Each attribute is applied only if the reply
 * carries it, and several applications additionally invalidate caches
 * that the change makes untrustworthy.
 */

/* A fattr that clears the identity guards and carries nothing else. */
static void init_update_fattr(struct nfs_inode_fixture *f,
			      struct nfs_fattr *fattr)
{
	memset(fattr, 0, sizeof(*fattr));
	fattr->valid = NFS_ATTR_FATTR_FILEID;
	fattr->fileid = f->nfsi.fileid;
	fattr->time_start = jiffies;
}

static struct nfs_inode_fixture *update_fixture(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 100, 0);
	struct inode *inode = fixture_inode(f);
	struct timespec64 ts = { .tv_sec = 1000, .tv_nsec = 0 };

	inode->i_mode = S_IFREG | 0644;
	set_nlink(inode, 1);
	i_size_write(inode, 4096);
	inode_set_mtime_to_ts(inode, ts);
	inode_set_ctime_to_ts(inode, ts);
	inode_set_atime_to_ts(inode, ts);
	f->nfsi.fileid = 1000;
	f->nfsi.cache_validity = 0;
	fixture_set_nrpages(f, 1);
	fixture_add_iostats(test, f);

	return f;
}

static void update_applies_mtime(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_MTIME;
	fattr.mtime = (struct timespec64){ .tv_sec = 2000, .tv_nsec = 0 };

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, inode_get_mtime_sec(fixture_inode(f)), 2000LL);
}

static void update_applies_ctime_and_atime(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_CTIME | NFS_ATTR_FATTR_ATIME;
	fattr.ctime = (struct timespec64){ .tv_sec = 2000, .tv_nsec = 0 };
	fattr.atime = (struct timespec64){ .tv_sec = 3000, .tv_nsec = 0 };

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, inode_get_ctime_sec(fixture_inode(f)), 2000LL);
	KUNIT_EXPECT_EQ(test, inode_get_atime_sec(fixture_inode(f)), 3000LL);
}

/*
 * A size change means the cached data no longer describes the file, so
 * applying it also invalidates the data cache.
 */
static void update_applies_size_and_invalidates_data(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_SIZE;
	fattr.size = 8192;

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, i_size_read(fixture_inode(f)), 8192LL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
}

/*
 * With writes in flight the server's smaller size is behind us and must
 * not shrink the file...
 */
static void update_does_not_shrink_file_with_writebacks(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	atomic_long_set(&f->nfsi.nrequests, 1);

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_SIZE;
	fattr.size = 1024;

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ_MSG(test, i_size_read(fixture_inode(f)), 4096LL,
			    "pending writebacks did not protect the file size");
}

/* ...but a larger size still wins, since the file grew on the server. */
static void update_still_grows_file_with_writebacks(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	atomic_long_set(&f->nfsi.nrequests, 1);

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_SIZE;
	fattr.size = 16384;

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, i_size_read(fixture_inode(f)), 16384LL);
}

/*
 * Only the permission bits are taken from the reply; the file-type bits
 * of the inode are preserved. Getting this wrong would rewrite the
 * inode's type from an attribute update.
 */
static void update_applies_only_permission_bits_of_mode(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;
	struct inode *inode = fixture_inode(f);

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_MODE;
	fattr.mode = 0600;	/* permissions only, no type bits */

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, inode->i_mode & S_IALLUGO, (umode_t)0600);
	KUNIT_EXPECT_EQ_MSG(test, inode->i_mode & S_IFMT, (umode_t)S_IFREG,
			    "the inode's file type was overwritten");
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACL,
			(unsigned long)NFS_INO_INVALID_ACL);
}

/* An unchanged mode must not invalidate the access and ACL caches. */
static void update_unchanged_mode_invalidates_nothing(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_MODE;
	fattr.mode = S_IFREG | 0644;	/* identical to the inode */

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			    0UL,
			    "an unchanged mode invalidated the access cache");
}

static void update_applies_owner_and_invalidates_access(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_OWNER;
	fattr.uid = KUIDT_INIT(4242);

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_TRUE(test,
			  uid_eq(fixture_inode(f)->i_uid, KUIDT_INIT(4242)));
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
}

static void update_applies_nlink(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_NLINK;
	fattr.nlink = 3;

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, fixture_inode(f)->i_nlink, 3U);
}

/* Space used is reported in bytes and stored in 512-byte units. */
static void update_converts_space_used_to_blocks(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	init_update_fattr(f, &fattr);
	fattr.valid |= NFS_ATTR_FATTR_SPACE_USED;
	fattr.du.nfs3.used = 8192;

	KUNIT_ASSERT_EQ(test, call_update_inode(f, &fattr), 0);
	KUNIT_EXPECT_EQ(test, fixture_inode(f)->i_blocks, 16UL);
}

/*
 * nfs_setattr_update_inode(): applying a local setattr
 */

static void setattr_update_applies_mode(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;
	struct iattr attr;

	nfs_fattr_init(&fattr);
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = 0600;

	nfs_setattr_update_inode(inode, &attr, &fattr);

	KUNIT_EXPECT_EQ(test, inode->i_mode & S_IALLUGO, (umode_t)0600);
	KUNIT_EXPECT_EQ_MSG(test, inode->i_mode & S_IFMT, (umode_t)S_IFREG,
			    "setattr overwrote the inode's file type");
}

static void setattr_update_applies_owner(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);
	struct nfs_fattr fattr;
	struct iattr attr;

	nfs_fattr_init(&fattr);
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_UID | ATTR_GID;
	attr.ia_uid = KUIDT_INIT(1234);
	attr.ia_gid = KGIDT_INIT(5678);

	nfs_setattr_update_inode(inode, &attr, &fattr);

	KUNIT_EXPECT_TRUE(test, uid_eq(inode->i_uid, KUIDT_INIT(1234)));
	KUNIT_EXPECT_TRUE(test, gid_eq(inode->i_gid, KGIDT_INIT(5678)));
}

/* Changing ownership invalidates the access and ACL caches. */
static void setattr_update_invalidates_access_on_chown(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;
	struct iattr attr;

	nfs_fattr_init(&fattr);
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_UID;
	attr.ia_uid = KUIDT_INIT(1234);

	nfs_setattr_update_inode(fixture_inode(f), &attr, &fattr);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
}

/* setattr installs a barrier so racing older replies cannot undo it. */
static void setattr_update_sets_a_barrier(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;
	struct iattr attr;
	unsigned long before;

	nfs_fattr_init(&fattr);
	before = fattr.gencount;
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = 0600;

	nfs_setattr_update_inode(fixture_inode(f), &attr, &fattr);

	KUNIT_EXPECT_NE(test, fattr.gencount, before);
	KUNIT_EXPECT_EQ(test, f->nfsi.attr_gencount, fattr.gencount);
}

/*
 * Timestamp helpers
 */

/* An explicit utimes() value is stored and its cache bit cleared. */
static void set_timestamps_applies_explicit_atime(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);
	struct iattr attr;

	f->nfsi.cache_validity = NFS_INO_INVALID_ATIME;

	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_ATIME_SET;
	attr.ia_atime = (struct timespec64){ .tv_sec = 7000, .tv_nsec = 0 };

	nfs_set_timestamps_to_ts(inode, &attr);

	KUNIT_EXPECT_EQ(test, inode_get_atime_sec(inode), 7000LL);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_ATIME, 0UL,
			    "atime was set but its cache bit was left invalid");
}

static void set_timestamps_applies_explicit_mtime(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);
	struct iattr attr;

	f->nfsi.cache_validity = NFS_INO_INVALID_MTIME |
				 NFS_INO_INVALID_CTIME;

	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_MTIME_SET;
	attr.ia_mtime = (struct timespec64){ .tv_sec = 7000, .tv_nsec = 0 };

	nfs_set_timestamps_to_ts(inode, &attr);

	KUNIT_EXPECT_EQ(test, inode_get_mtime_sec(inode), 7000LL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_MTIME,
			0UL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_CTIME,
			0UL);
}

/* Nothing requested means nothing touched. */
static void set_timestamps_without_flags_changes_nothing(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct iattr attr;

	f->nfsi.cache_validity = NFS_INO_INVALID_ATIME;

	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = 0;

	nfs_set_timestamps_to_ts(fixture_inode(f), &attr);

	KUNIT_EXPECT_EQ(test, inode_get_atime_sec(fixture_inode(f)), 1000LL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATIME,
			(unsigned long)NFS_INO_INVALID_ATIME);
}

/*
 * An implicit mtime update also refreshes ctime, so both cache bits are
 * cleared together.
 */
static void update_timestamps_mtime_also_clears_ctime(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = NFS_INO_INVALID_MTIME |
				 NFS_INO_INVALID_CTIME |
				 NFS_INO_INVALID_ATIME;

	nfs_update_timestamps(fixture_inode(f), ATTR_MTIME);

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_MTIME,
			0UL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_CTIME,
			0UL);
	KUNIT_EXPECT_EQ_MSG(test,
			    f->nfsi.cache_validity & NFS_INO_INVALID_ATIME,
			    (unsigned long)NFS_INO_INVALID_ATIME,
			    "an mtime update cleared the atime cache bit");
}

/*
 * nfs_ooo_record() and nfs_inode_finish_partial_attr_update()
 */

/* A reply with both change halves records the gap between them. */
static void ooo_record_captures_change_gap(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE | NFS_ATTR_FATTR_PRECHANGE;
	fattr.change_attr = 10;
	fattr.pre_change_attr = 20;

	nfs_ooo_record(&f->nfsi, &fattr);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_EQ(test, f->nfsi.ooo->cnt, 1);
	kfree(f->nfsi.ooo);
}

/* Without both halves there is no gap to describe. */
static void ooo_record_ignores_incomplete_change_info(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_fattr fattr;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE;	/* no PRECHANGE */
	fattr.change_attr = 10;

	nfs_ooo_record(&f->nfsi, &fattr);

	KUNIT_EXPECT_PTR_EQ(test, f->nfsi.ooo, NULL);
}

/*
 * A reply with an unchanged change attribute can still finish a partial
 * update: the change attribute proves nothing else moved, so the
 * attributes still marked invalid can be filled in from it.
 */
static void finish_partial_update_when_change_is_unmoved(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	f->nfsi.cache_validity = NFS_INO_INVALID_SIZE;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE;
	fattr.change_attr = 100;	/* equal to the inode's */

	KUNIT_EXPECT_EQ(test,
			nfs_inode_finish_partial_attr_update(&fattr,
							     fixture_inode(f)),
			1);
}

/* If the change attribute itself is suspect, nothing can be concluded. */
static void finish_partial_update_declines_when_change_invalid(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	f->nfsi.cache_validity = NFS_INO_INVALID_SIZE |
				 NFS_INO_INVALID_CHANGE;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE;
	fattr.change_attr = 100;

	KUNIT_EXPECT_EQ(test,
			nfs_inode_finish_partial_attr_update(&fattr,
							     fixture_inode(f)),
			0);
}

/* With nothing outstanding there is no partial update to finish. */
static void finish_partial_update_declines_when_cache_is_clean(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_TIME_METADATA,
				  100, 0);
	struct nfs_fattr fattr;

	f->nfsi.cache_validity = 0;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_CHANGE;
	fattr.change_attr = 100;

	KUNIT_EXPECT_EQ(test,
			nfs_inode_finish_partial_attr_update(&fattr,
							     fixture_inode(f)),
			0);
}

/*
 * Inode cache matching
 *
 * nfs_find_actor() is the predicate iget5_locked() uses to decide whether
 * a cached inode is the object a reply describes. Every one of its four
 * checks is a reason to reject a candidate, and a false positive here
 * hands back the wrong inode entirely -- so each rejection is tested on
 * its own.
 *
 * The callback is pure, so it can be driven directly without going
 * anywhere near the inode cache.
 */

static void find_desc_init(struct nfs_find_desc *desc, struct nfs_fh *fh,
			   struct nfs_fattr *fattr, u64 fileid, umode_t mode)
{
	memset(fh, 0, sizeof(*fh));
	fh->size = 8;
	memset(fh->data, 0xab, fh->size);

	memset(fattr, 0, sizeof(*fattr));
	fattr->fileid = fileid;
	fattr->mode = mode;

	desc->fh = fh;
	desc->fattr = fattr;
}

/* A candidate agreeing on fileid, type and filehandle is a match. */
static void find_actor_matches_identical_inode(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 1000, S_IFREG | 0644);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;
	nfs_copy_fh(&f->nfsi.fh, &fh);

	KUNIT_EXPECT_EQ(test, nfs_find_actor(fixture_inode(f), &desc), 1);
}

static void find_actor_rejects_different_fileid(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 2000, S_IFREG | 0644);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;
	nfs_copy_fh(&f->nfsi.fh, &fh);

	KUNIT_EXPECT_EQ_MSG(test, nfs_find_actor(fixture_inode(f), &desc), 0,
			    "an inode with a different fileid was matched");
}

static void find_actor_rejects_different_type(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 1000, S_IFDIR | 0755);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;
	nfs_copy_fh(&f->nfsi.fh, &fh);

	KUNIT_EXPECT_EQ_MSG(test, nfs_find_actor(fixture_inode(f), &desc), 0,
			    "a regular file was matched against a directory");
}

/*
 * Two different files on the server can share a fileid across
 * filesystems, so the filehandle is the real identity and must be
 * compared even when everything else agrees.
 */
static void find_actor_rejects_different_filehandle(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 1000, S_IFREG | 0644);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;

	/* Same length, different contents. */
	nfs_copy_fh(&f->nfsi.fh, &fh);
	f->nfsi.fh.data[0] = 0x00;

	KUNIT_EXPECT_EQ_MSG(test, nfs_find_actor(fixture_inode(f), &desc), 0,
			    "inodes with different filehandles were matched");
}

/* A stale inode must never be handed back, however well it matches. */
static void find_actor_rejects_stale_inode(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 1000, S_IFREG | 0644);
	fixture_inode(f)->i_mode = S_IFREG | 0644;
	f->nfsi.fileid = 1000;
	nfs_copy_fh(&f->nfsi.fh, &fh);
	set_bit(NFS_INO_STALE, &f->nfsi.flags);

	KUNIT_EXPECT_EQ_MSG(test, nfs_find_actor(fixture_inode(f), &desc), 0,
			    "a stale inode was reused");
}

/* nfs_init_locked() seeds a freshly allocated inode from the reply. */
static void init_locked_seeds_identity(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 4321, S_IFDIR | 0755);

	KUNIT_ASSERT_EQ(test, nfs_init_locked(fixture_inode(f), &desc), 0);

	KUNIT_EXPECT_EQ(test, f->nfsi.fileid, 4321ULL);
	KUNIT_EXPECT_EQ(test, fixture_inode(f)->i_mode,
			(umode_t)(S_IFDIR | 0755));
	KUNIT_EXPECT_EQ(test, nfs_compare_fh(&f->nfsi.fh, &fh), 0);
}

/*
 * An inode seeded by nfs_init_locked() must then be recognised by
 * nfs_find_actor() using the same descriptor. If these two disagreed the
 * cache would miss on every lookup and allocate endlessly.
 */
static void init_locked_result_is_findable(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);
	struct nfs_find_desc desc;
	struct nfs_fattr fattr;
	struct nfs_fh fh;

	find_desc_init(&desc, &fh, &fattr, 4321, S_IFREG | 0644);

	KUNIT_ASSERT_EQ(test, nfs_init_locked(fixture_inode(f), &desc), 0);
	KUNIT_EXPECT_EQ_MSG(test, nfs_find_actor(fixture_inode(f), &desc), 1,
			    "an inode just seeded from a reply did not match it");
}

/*
 * readdirplus heuristics
 *
 * Asking for readdirplus is only worthwhile when the server supports it,
 * no writes are in flight, and attributes are cached long enough for the
 * extra data to still be useful when read.
 */

static void readdirplus_enabled_when_all_conditions_hold(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_inode(f)->i_mode = S_IFDIR | 0755;
	f->server.caps = NFS_CAP_READDIRPLUS;
	f->server.acdirmax = 60 * HZ;

	KUNIT_EXPECT_TRUE(test,
			  nfs_getattr_readdirplus_enable(fixture_inode(f)));
}

static void readdirplus_disabled_without_server_support(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_inode(f)->i_mode = S_IFDIR | 0755;
	f->server.caps = 0;
	f->server.acdirmax = 60 * HZ;

	KUNIT_EXPECT_FALSE(test,
			   nfs_getattr_readdirplus_enable(fixture_inode(f)));
}

/*
 * A short attribute timeout means the extra attributes would expire
 * before they were used, so the optimisation is skipped.
 */
static void readdirplus_disabled_with_short_attr_timeout(struct kunit *test)
{
	struct nfs_inode_fixture *f =
		nfs_inode_fixture(test, NFS4_CHANGE_TYPE_IS_UNDEFINED, 0, 0);

	fixture_inode(f)->i_mode = S_IFDIR | 0755;
	f->server.caps = NFS_CAP_READDIRPLUS;
	f->server.acdirmax = 5 * HZ;	/* not greater than the threshold */

	KUNIT_EXPECT_FALSE(test,
			   nfs_getattr_readdirplus_enable(fixture_inode(f)));
}

/*
 * Revalidation
 *
 * __nfs_revalidate_inode() is where the client goes back to the server
 * for fresh attributes. It reaches the wire through
 * NFS_PROTO(inode)->getattr, a function pointer, so a stub stands in for
 * the whole RPC layer -- no server, no network, and complete control over
 * what the "server" returns.
 *
 * That control is the point: the interesting logic here is entirely in
 * the error paths, and those are exactly the ones that are awkward to
 * provoke against a live server.
 */

static int getattr_result;
static int getattr_calls;
static u64 getattr_fileid;

static int stub_getattr(struct nfs_server *server, struct nfs_fh *fh,
			struct nfs_fattr *fattr, struct inode *inode)
{
	getattr_calls++;
	if (getattr_result == 0) {
		fattr->valid = NFS_ATTR_FATTR_FILEID | NFS_ATTR_FATTR_TYPE;
		fattr->fileid = getattr_fileid;
		fattr->mode = inode->i_mode;
		fattr->time_start = jiffies;
	}
	return getattr_result;
}

static struct nfs_inode_fixture *revalidate_fixture(struct kunit *test,
						    int result)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	getattr_result = result;
	getattr_calls = 0;
	getattr_fileid = f->nfsi.fileid;
	f->rpc_ops.getattr = stub_getattr;

	return f;
}

static void revalidate_succeeds_and_queries_the_server(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, 0);

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			0);
	KUNIT_EXPECT_EQ_MSG(test, getattr_calls, 1,
			    "revalidation did not issue a GETATTR");
}

/*
 * A stale reply for a regular file marks the inode stale, so later
 * lookups know not to reuse it.
 */
static void revalidate_stale_marks_regular_file_stale(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, -ESTALE);

	fixture_inode(f)->i_mode = S_IFREG | 0644;

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			-ESTALE);
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_INO_STALE, &f->nfsi.flags));
}

/*
 * A directory is treated differently: it is not marked stale, only its
 * caches are dropped, because a directory whose contents changed is still
 * the same directory.
 */
static void revalidate_stale_only_zaps_directory(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, -ESTALE);

	fixture_inode(f)->i_mode = S_IFDIR | 0755;
	f->nfsi.cache_validity = 0;

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			-ESTALE);
	KUNIT_EXPECT_FALSE_MSG(test, test_bit(NFS_INO_STALE, &f->nfsi.flags),
			       "a directory was marked stale rather than zapped");
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ATTR,
			(unsigned long)NFS_INO_INVALID_ATTR);
}

/*
 * On a soft-revalidate mount a timeout is not an error: the cached
 * attributes are kept and the caller carries on.
 */
static void revalidate_timeout_is_tolerated_with_softreval(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, -ETIMEDOUT);

	f->server.flags = NFS_MOUNT_SOFTREVAL;

	KUNIT_EXPECT_EQ_MSG(test,
			    __nfs_revalidate_inode(&f->server,
						   fixture_inode(f)), 0,
			    "SOFTREVAL did not absorb the timeout");
}

/* Without that flag the timeout is reported to the caller. */
static void revalidate_timeout_propagates_without_softreval(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, -ETIMEDOUT);

	f->server.flags = 0;

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			-ETIMEDOUT);
}

/* Any other error is passed straight back. */
static void revalidate_other_errors_propagate(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, -EACCES);

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			-EACCES);
}

/*
 * An inode already known to be stale is not worth asking about, so the
 * server is never contacted.
 */
static void revalidate_skips_already_stale_inode(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, 0);

	set_bit(NFS_INO_STALE, &f->nfsi.flags);

	KUNIT_EXPECT_EQ(test,
			__nfs_revalidate_inode(&f->server, fixture_inode(f)),
			-ESTALE);
	KUNIT_EXPECT_EQ_MSG(test, getattr_calls, 0,
			    "a known-stale inode was revalidated anyway");
}

/*
 * nfs_revalidate_inode() is the gate above it: a cache that is still
 * valid means no round trip at all.
 */
static void revalidate_gate_skips_when_cache_is_valid(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, 0);

	f->nfsi.cache_validity = 0;
	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;

	KUNIT_EXPECT_EQ(test,
			nfs_revalidate_inode(fixture_inode(f),
					     NFS_INO_INVALID_ACCESS), 0);
	KUNIT_EXPECT_EQ_MSG(test, getattr_calls, 0,
			    "a valid cache still triggered a GETATTR");
}

static void revalidate_gate_queries_when_flag_is_invalid(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, 0);

	f->nfsi.cache_validity = NFS_INO_INVALID_ACCESS;
	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;

	KUNIT_EXPECT_EQ(test,
			nfs_revalidate_inode(fixture_inode(f),
					     NFS_INO_INVALID_ACCESS), 0);
	KUNIT_EXPECT_EQ(test, getattr_calls, 1);
}

/* A stale inode short-circuits the gate with -ESTALE and no round trip. */
static void revalidate_gate_reports_stale_without_querying(struct kunit *test)
{
	struct nfs_inode_fixture *f = revalidate_fixture(test, 0);

	f->nfsi.cache_validity = 0;
	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;
	set_bit(NFS_INO_STALE, &f->nfsi.flags);

	KUNIT_EXPECT_EQ(test,
			nfs_revalidate_inode(fixture_inode(f),
					     NFS_INO_INVALID_ACCESS), -ESTALE);
	KUNIT_EXPECT_EQ(test, getattr_calls, 0);
}

static void mapping_needs_revalidate_when_change_is_invalid(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = NFS_INO_INVALID_CHANGE;

	KUNIT_EXPECT_TRUE(test,
			  nfs_mapping_need_revalidate_inode(fixture_inode(f)));
}

static void mapping_needs_revalidate_when_stale(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = 0;
	f->nfsi.read_cache_jiffies = jiffies;
	f->nfsi.attrtimeo = 60 * HZ;
	set_bit(NFS_INO_STALE, &f->nfsi.flags);

	KUNIT_EXPECT_TRUE(test,
			  nfs_mapping_need_revalidate_inode(fixture_inode(f)));
}

/*
 * Writeback and sync
 *
 * nfs_sync_inode() looked like the one thing here that genuinely needs
 * the VM: it waits for direct I/O, flushes the mapping and commits. It
 * turns out every one of those has a cheap exit when there is nothing
 * outstanding -- inode_dio_wait() returns at once with i_dio_count zero,
 * filemap_write_and_wait() skips writeback when nrpages is zero, and the
 * commit loop terminates immediately on an empty commit list.
 *
 * So the clean-inode path is reachable. What is *not* reachable is the
 * interesting case: actually flushing dirty pages needs real page cache
 * state, and no amount of struct-filling substitutes for it. These tests
 * therefore pin the no-op path only, which is still worth having -- if a
 * guard regressed, this is where it would hang rather than return.
 */

static struct nfs_inode_fixture *sync_fixture(struct kunit *test,
					      umode_t mode)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);

	inode->i_mode = mode;
	if (S_ISREG(mode))
		nfs_inode_init_regular(&f->nfsi);
	else if (S_ISDIR(mode))
		nfs_inode_init_dir(&f->nfsi);

	/* Nothing dirty, nothing in flight, nothing to commit. */
	fixture_set_nrpages(f, 0);
	atomic_set(&inode->i_dio_count, 0);

	return f;
}

/* A clean regular file syncs to success without doing any work. */
static void sync_inode_on_clean_regular_file_succeeds(struct kunit *test)
{
	struct nfs_inode_fixture *f = sync_fixture(test, S_IFREG | 0644);

	KUNIT_EXPECT_EQ(test, nfs_sync_inode(fixture_inode(f)), 0);
}

static void sync_inode_on_clean_directory_succeeds(struct kunit *test)
{
	struct nfs_inode_fixture *f = sync_fixture(test, S_IFDIR | 0755);

	KUNIT_EXPECT_EQ(test, nfs_sync_inode(fixture_inode(f)), 0);
}

/* Committing an inode with an empty commit list is a no-op. */
static void commit_inode_with_nothing_pending_succeeds(struct kunit *test)
{
	struct nfs_inode_fixture *f = sync_fixture(test, S_IFREG | 0644);

	KUNIT_EXPECT_EQ(test, nfs_commit_inode(fixture_inode(f), 0), 0);
	KUNIT_EXPECT_EQ_MSG(test,
			    atomic_read(&f->nfsi.commit_info.rpcs_out), 0,
			    "commit accounting was left unbalanced");
}

/*
 * The commit counter must come back to zero afterwards; a leak here would
 * make a later synchronous commit wait forever.
 */
static void commit_inode_balances_its_counter(struct kunit *test)
{
	struct nfs_inode_fixture *f = sync_fixture(test, S_IFREG | 0644);
	int i;

	for (i = 0; i < 3; i++)
		KUNIT_ASSERT_EQ(test, nfs_commit_inode(fixture_inode(f), 0), 0);

	KUNIT_EXPECT_EQ(test, atomic_read(&f->nfsi.commit_info.rpcs_out), 0);
}

/*
 * nfs_sync_mapping() short-circuits entirely when no pages are cached,
 * which is the guard that keeps it away from unmap_mapping_range().
 */
static void sync_mapping_without_pages_is_a_noop(struct kunit *test)
{
	struct nfs_inode_fixture *f = sync_fixture(test, S_IFREG | 0644);

	f->mapping.host = fixture_inode(f);
	fixture_set_nrpages(f, 0);

	KUNIT_EXPECT_EQ(test, nfs_sync_mapping(&f->mapping), 0);
}

/*
 * Inode lifetime
 */

/*
 * A stale inode is dropped on last reference rather than kept in the
 * cache, since nothing about it can be trusted any more. This holds even
 * when the generic rules would have retained it.
 */
static void drop_inode_drops_stale_inode(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);

	set_nlink(inode, 1);
	/* Pretend the inode is hashed, so generic_drop_inode() would keep it. */
	inode->i_hash.pprev = &inode->i_hash.next;

	KUNIT_ASSERT_EQ_MSG(test, nfs_drop_inode(inode), 0,
			    "a healthy hashed inode was dropped");

	set_bit(NFS_INO_STALE, &f->nfsi.flags);
	KUNIT_EXPECT_NE_MSG(test, nfs_drop_inode(inode), 0,
			    "a stale inode was retained in the cache");
}

/* An unlinked inode is dropped by the generic rule. */
static void drop_inode_drops_unlinked_inode(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct inode *inode = fixture_inode(f);

	inode->i_hash.pprev = &inode->i_hash.next;
	clear_nlink(inode);

	KUNIT_EXPECT_NE(test, nfs_drop_inode(inode), 0);
}

/*
 * Delegated timestamps
 *
 * Holding a delegation on the times means the client's copies are
 * authoritative. Server-supplied timestamps are therefore stripped from
 * the reply before it is applied -- but only for the times whose caches
 * are still believed valid. A time already marked invalid must survive,
 * because the client has admitted it does not know the answer.
 */

static void fixup_delegated_strips_times_under_mtime_delegation(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	f->rpc_ops.have_delegation = stub_has_delegation;
	f->nfsi.cache_validity = 0;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_MTIME | NFS_ATTR_FATTR_CTIME |
		      NFS_ATTR_FATTR_ATIME | NFS_ATTR_FATTR_SIZE;

	nfs_fattr_fixup_delegated(fixture_inode(f), &fattr);

	KUNIT_EXPECT_EQ(test, fattr.valid & NFS_ATTR_FATTR_MTIME, 0U);
	KUNIT_EXPECT_EQ(test, fattr.valid & NFS_ATTR_FATTR_CTIME, 0U);
	KUNIT_EXPECT_EQ(test, fattr.valid & NFS_ATTR_FATTR_ATIME, 0U);
	KUNIT_EXPECT_EQ_MSG(test, fattr.valid & NFS_ATTR_FATTR_SIZE,
			    (unsigned int)NFS_ATTR_FATTR_SIZE,
			    "a delegation stripped a non-timestamp attribute");
}

/*
 * A timestamp the client has already marked invalid is kept, since the
 * delegation is not a substitute for knowledge the client has discarded.
 */
static void fixup_delegated_keeps_times_it_admits_are_invalid(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;

	f->rpc_ops.have_delegation = stub_has_delegation;
	f->nfsi.cache_validity = NFS_INO_INVALID_MTIME;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_MTIME | NFS_ATTR_FATTR_CTIME;

	nfs_fattr_fixup_delegated(fixture_inode(f), &fattr);

	KUNIT_EXPECT_EQ_MSG(test, fattr.valid & NFS_ATTR_FATTR_MTIME,
			    (unsigned int)NFS_ATTR_FATTR_MTIME,
			    "an mtime the client knew was stale got stripped");
	KUNIT_EXPECT_EQ(test, fattr.valid & NFS_ATTR_FATTR_CTIME, 0U);
}

/* Without a delegation nothing is stripped at all. */
static void fixup_delegated_is_a_noop_without_delegation(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);
	struct nfs_fattr fattr;
	unsigned int before;

	f->nfsi.cache_validity = 0;

	memset(&fattr, 0, sizeof(fattr));
	fattr.valid = NFS_ATTR_FATTR_MTIME | NFS_ATTR_FATTR_CTIME |
		      NFS_ATTR_FATTR_ATIME;
	before = fattr.valid;

	nfs_fattr_fixup_delegated(fixture_inode(f), &fattr);

	KUNIT_EXPECT_EQ(test, fattr.valid, before);
}

/*
 * Buffered writers
 *
 * nfs_file_has_buffered_writers() is nfs_file_has_writers() qualified by
 * the file not being in O_DIRECT mode, since direct I/O bypasses the
 * page cache and so cannot hold back a size update.
 */
static void buffered_writers_false_when_file_is_odirect(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	set_bit(NFS_INO_ODIRECT, &f->nfsi.flags);

	KUNIT_EXPECT_FALSE(test, nfs_file_has_buffered_writers(&f->nfsi));
}

static void buffered_writers_false_without_open_files(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	clear_bit(NFS_INO_ODIRECT, &f->nfsi.flags);

	KUNIT_EXPECT_FALSE(test, nfs_file_has_buffered_writers(&f->nfsi));
}

/*
 * Lock contexts
 *
 * A lock context is keyed on the opening task's file table, so that
 * locks taken by unrelated processes through the same open file are kept
 * apart.
 */

static void init_lock_context_starts_referenced_and_idle(struct kunit *test)
{
	struct nfs_lock_context *l_ctx;

	l_ctx = kunit_kzalloc(test, sizeof(*l_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, l_ctx);

	nfs_init_lock_context(l_ctx);

	KUNIT_EXPECT_EQ(test, refcount_read(&l_ctx->count), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&l_ctx->io_count), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&l_ctx->list));
	KUNIT_EXPECT_PTR_EQ(test, l_ctx->lockowner, (void *)current->files);
}

static void find_lock_context_returns_null_when_empty(struct kunit *test)
{
	struct nfs_open_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	nfs_init_lock_context(&ctx->lock_context);

	KUNIT_EXPECT_PTR_EQ(test, __nfs_find_lock_context(ctx), NULL);
}

/* A context belonging to this task is found, and takes a reference. */
static void find_lock_context_matches_current_owner(struct kunit *test)
{
	struct nfs_open_context *ctx;
	struct nfs_lock_context *l_ctx, *found;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	l_ctx = kunit_kzalloc(test, sizeof(*l_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, l_ctx);

	nfs_init_lock_context(&ctx->lock_context);
	nfs_init_lock_context(l_ctx);
	list_add_tail_rcu(&l_ctx->list, &ctx->lock_context.list);

	found = __nfs_find_lock_context(ctx);
	KUNIT_EXPECT_PTR_EQ(test, found, l_ctx);
	KUNIT_EXPECT_EQ_MSG(test, refcount_read(&l_ctx->count), 2,
			    "finding a lock context did not take a reference");

	list_del_rcu(&l_ctx->list);
}

/* A context owned by a different file table is not a match. */
static void find_lock_context_skips_other_owner(struct kunit *test)
{
	struct nfs_open_context *ctx;
	struct nfs_lock_context *l_ctx;
	unsigned long other_owner;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	l_ctx = kunit_kzalloc(test, sizeof(*l_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, l_ctx);

	nfs_init_lock_context(&ctx->lock_context);
	nfs_init_lock_context(l_ctx);
	l_ctx->lockowner = (fl_owner_t)&other_owner;
	list_add_tail_rcu(&l_ctx->list, &ctx->lock_context.list);

	KUNIT_EXPECT_PTR_EQ_MSG(test, __nfs_find_lock_context(ctx), NULL,
				"a lock context from another owner was returned");

	list_del_rcu(&l_ctx->list);
}

/*
 * Open contexts
 *
 * An open context records who opened a file and how. It is reference
 * counted through its embedded lock context, hangs off the inode's
 * open_files list, and is looked up by credential and access mode.
 *
 * These need a dentry, but only for its d_inode and d_sb pointers, so a
 * zeroed struct with those two fields set is enough.
 */

struct open_ctx_fixture {
	struct nfs_inode_fixture	*f;
	struct dentry			dentry;
	struct nfs_open_context		ctx;
};

static struct open_ctx_fixture *open_ctx_fixture(struct kunit *test,
						 fmode_t mode)
{
	struct open_ctx_fixture *o;

	o = kunit_kzalloc(test, sizeof(*o), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, o);

	o->f = update_fixture(test);
	o->dentry.d_inode = fixture_inode(o->f);
	o->dentry.d_sb = &o->f->sb;

	o->ctx.dentry = &o->dentry;
	o->ctx.cred = current_cred();
	o->ctx.mode = mode;
	o->ctx.flags = 0;
	nfs_init_lock_context(&o->ctx.lock_context);
	o->ctx.lock_context.open_context = &o->ctx;
	INIT_LIST_HEAD(&o->ctx.list);

	return o;
}

/* Taking a reference on a live context returns it and bumps the count. */
static void get_open_context_takes_a_reference(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	KUNIT_EXPECT_PTR_EQ(test, get_nfs_open_context(&o->ctx), &o->ctx);
	KUNIT_EXPECT_EQ(test, refcount_read(&o->ctx.lock_context.count), 2);
}

static void get_open_context_tolerates_null(struct kunit *test)
{
	KUNIT_EXPECT_PTR_EQ(test, get_nfs_open_context(NULL), NULL);
}

/*
 * A context whose count has already reached zero is being torn down and
 * must not be resurrected. refcount_inc_not_zero() is what enforces
 * that, and getting it wrong would hand out a freed context.
 */
static void get_open_context_refuses_dead_context(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	refcount_set(&o->ctx.lock_context.count, 0);

	KUNIT_EXPECT_PTR_EQ_MSG(test, get_nfs_open_context(&o->ctx), NULL,
				"a context being torn down was handed out");

	refcount_set(&o->ctx.lock_context.count, 1);
}

/* Attaching a context puts it on the inode's open file list. */
static void attach_open_context_links_it_to_the_inode(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	KUNIT_ASSERT_TRUE(test, list_empty(&o->f->nfsi.open_files));

	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_FALSE(test, list_empty(&o->f->nfsi.open_files));
	list_del_rcu(&o->ctx.list);
}

/*
 * Opening a file that has unmerged out-of-order gaps means the data
 * cache cannot be trusted, so the first attach invalidates it. Without
 * outstanding gaps there is nothing to invalidate.
 */
static void attach_open_context_invalidates_data_after_reordering(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	o->f->nfsi.cache_validity = NFS_INO_DATA_INVAL_DEFER;
	fixture_set_nrpages(o->f, 1);

	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_EQ(test,
			o->f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			(unsigned long)NFS_INO_INVALID_DATA);
	list_del_rcu(&o->ctx.list);
}

static void attach_open_context_leaves_clean_cache_alone(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	o->f->nfsi.cache_validity = 0;
	o->f->nfsi.ooo = NULL;
	fixture_set_nrpages(o->f, 1);

	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_EQ_MSG(test,
			    o->f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			    0UL,
			    "attaching to a clean inode invalidated its data");
	list_del_rcu(&o->ctx.list);
}

/*
 * nfs_find_open_context() matches on credential, exact access mode, and
 * the context still being open. Each is a separate reason to skip a
 * candidate.
 */
static void find_open_context_matches_mode_and_cred(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);
	struct nfs_open_context *found;

	set_bit(NFS_CONTEXT_FILE_OPEN, &o->ctx.flags);
	nfs_inode_attach_open_context(&o->ctx);

	found = nfs_find_open_context(fixture_inode(o->f), o->ctx.cred,
				      FMODE_READ);
	KUNIT_EXPECT_PTR_EQ(test, found, &o->ctx);

	list_del_rcu(&o->ctx.list);
}

/* A read context is not a match for a caller wanting write access. */
static void find_open_context_requires_exact_mode(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	set_bit(NFS_CONTEXT_FILE_OPEN, &o->ctx.flags);
	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_PTR_EQ_MSG(test,
				nfs_find_open_context(fixture_inode(o->f),
						      o->ctx.cred, FMODE_WRITE),
				NULL,
				"a read-only context satisfied a write request");

	list_del_rcu(&o->ctx.list);
}

/* A context whose file has been closed is skipped. */
static void find_open_context_skips_closed_context(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	clear_bit(NFS_CONTEXT_FILE_OPEN, &o->ctx.flags);
	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_PTR_EQ_MSG(test,
				nfs_find_open_context(fixture_inode(o->f),
						      o->ctx.cred, FMODE_READ),
				NULL,
				"a closed context was returned as open");

	list_del_rcu(&o->ctx.list);
}

/* A NULL credential means "any", so only mode and open state matter. */
static void find_open_context_null_cred_matches_any(struct kunit *test)
{
	struct open_ctx_fixture *o = open_ctx_fixture(test, FMODE_READ);

	set_bit(NFS_CONTEXT_FILE_OPEN, &o->ctx.flags);
	nfs_inode_attach_open_context(&o->ctx);

	KUNIT_EXPECT_PTR_EQ(test,
			    nfs_find_open_context(fixture_inode(o->f), NULL,
						  FMODE_READ),
			    &o->ctx);

	list_del_rcu(&o->ctx.list);
}

/*
 * nfs_ooo_test(): are there unmerged out-of-order gaps?
 */
static void ooo_test_false_when_nothing_outstanding(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = 0;
	f->nfsi.ooo = NULL;

	KUNIT_EXPECT_FALSE(test, nfs_ooo_test(&f->nfsi));
}

static void ooo_test_true_when_deferred(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = NFS_INO_DATA_INVAL_DEFER;
	f->nfsi.ooo = NULL;

	KUNIT_EXPECT_TRUE(test, nfs_ooo_test(&f->nfsi));
}

static void ooo_test_true_when_gaps_recorded(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = 0;
	nfs_ooo_merge(&f->nfsi, 10, 20);

	KUNIT_EXPECT_TRUE(test, nfs_ooo_test(&f->nfsi));
	kfree(f->nfsi.ooo);
	f->nfsi.ooo = NULL;
}

/* An allocated but empty gap table is not an outstanding reordering. */
static void ooo_test_false_when_table_is_empty(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->nfsi.cache_validity = 0;
	nfs_ooo_merge(&f->nfsi, 42, 42);	/* empty range: allocates, records nothing */

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->nfsi.ooo);
	KUNIT_EXPECT_FALSE(test, nfs_ooo_test(&f->nfsi));
	kfree(f->nfsi.ooo);
	f->nfsi.ooo = NULL;
}

/* nfs_clear_inode() drops the ACL validity bit on the way out. */
static void clear_inode_drops_acl_validity(struct kunit *test)
{
	struct nfs_inode_fixture *f = update_fixture(test);

	f->rpc_ops.clear_acl_cache = NULL;
	f->nfsi.cache_validity = NFS_INO_INVALID_ACL |
				 NFS_INO_INVALID_ACCESS;

	nfs_clear_inode(fixture_inode(f));

	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACL,
			0UL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			(unsigned long)NFS_INO_INVALID_ACCESS);
}

static struct kunit_case nfs_attr_cmp_cases[] = {
	KUNIT_CASE(monotonic_larger_change_attr_is_newer),
	KUNIT_CASE(monotonic_equal_change_attr_is_unchanged),
	KUNIT_CASE(monotonic_smaller_change_attr_is_stale),
	KUNIT_CASE(strict_monotonic_equal_change_attr_is_stale),
	KUNIT_CASE(strict_monotonic_larger_change_attr_is_newer),
	KUNIT_CASE(undefined_change_type_is_undecided),
	KUNIT_CASE(missing_change_attr_is_undecided),
	KUNIT_CASE(newer_gencount_overrides_change_attr),
	{}
};

static struct kunit_suite nfs_attr_cmp_suite = {
	.name		= "nfs-inode-attr-cmp",
	.test_cases	= nfs_attr_cmp_cases,
};

static struct kunit_case nfs_cache_invalid_cases[] = {
	KUNIT_CASE(zap_mapping_without_pages_is_a_noop),
	KUNIT_CASE(zap_mapping_with_pages_invalidates_data),
	KUNIT_CASE(set_cache_invalid_strips_data_flag_without_pages),
	KUNIT_CASE(set_cache_invalid_accumulates_flags),
	KUNIT_CASE(set_cache_invalid_does_not_store_reval_forced),
	{}
};

static struct kunit_suite nfs_cache_invalid_suite = {
	.name		= "nfs-inode-cache-invalid",
	.test_cases	= nfs_cache_invalid_cases,
};

static struct kunit_case nfs_cache_expiry_cases[] = {
	KUNIT_CASE(attribute_timeout_within_window_is_fresh),
	KUNIT_CASE(attribute_timeout_past_window_has_expired),
	KUNIT_CASE(attribute_timeout_zero_expires_immediately),
	KUNIT_CASE(delegated_attributes_never_expire),
	KUNIT_CASE(check_cache_invalid_honours_flags),
	KUNIT_CASE(check_cache_flags_invalid_matches_any_bit),
	{}
};

static struct kunit_suite nfs_cache_expiry_suite = {
	.name		= "nfs-inode-cache-expiry",
	.test_cases	= nfs_cache_expiry_cases,
};

static struct kunit_case nfs_ooo_cases[] = {
	KUNIT_CASE(ooo_records_a_single_gap),
	KUNIT_CASE(ooo_merges_abutting_gap_below),
	KUNIT_CASE(ooo_merges_abutting_gap_above),
	KUNIT_CASE(ooo_keeps_disjoint_gaps_apart),
	KUNIT_CASE(ooo_empty_range_records_nothing),
	KUNIT_CASE(ooo_overflow_falls_back_to_defer),
	KUNIT_CASE(ooo_merge_is_skipped_once_deferred),
	{}
};

static struct kunit_suite nfs_ooo_suite = {
	.name		= "nfs-inode-out-of-order",
	.test_cases	= nfs_ooo_cases,
};

static struct kunit_case nfs_zap_cases[] = {
	KUNIT_CASE(zap_caches_invalidates_data_for_regular_files),
	KUNIT_CASE(zap_caches_withholds_data_for_special_files),
	KUNIT_CASE(invalidate_atime_sets_only_atime),
	{}
};

static struct kunit_suite nfs_zap_suite = {
	.name		= "nfs-inode-zap-caches",
	.test_cases	= nfs_zap_cases,
};

static struct kunit_case nfs_helper_cases[] = {
	KUNIT_CASE(fileid_to_ino_folds_high_bits),
	KUNIT_CASE(valid_attrmask_reports_everything_when_cache_is_good),
	KUNIT_CASE(valid_attrmask_drops_invalidated_fields),
	KUNIT_CASE(valid_attrmask_pairs_uid_and_gid),
	KUNIT_CASE(file_has_writers_is_false_for_non_regular),
	KUNIT_CASE(file_has_writers_is_false_without_open_files),
	KUNIT_CASE(zap_acl_cache_calls_protocol_hook_and_clears_flag),
	KUNIT_CASE(zap_acl_cache_tolerates_absent_hook),
	KUNIT_CASE(set_inode_stale_flags_and_zaps),
	{}
};

static struct kunit_suite nfs_helper_suite = {
	.name		= "nfs-inode-helpers",
	.test_cases	= nfs_helper_cases,
};

static struct kunit_case nfs_alloc_cases[] = {
	KUNIT_CASE(alloc_fattr_starts_invalid),
	KUNIT_CASE(fattr_init_advances_generation_counter),
	KUNIT_CASE(fattr_set_barrier_advances_generation),
	KUNIT_CASE(alloc_fhandle_starts_empty),
	{}
};

static struct kunit_suite nfs_alloc_suite = {
	.name		= "nfs-inode-alloc",
	.test_cases	= nfs_alloc_cases,
};

static struct kunit_case nfs_update_inode_cases[] = {
	KUNIT_CASE(update_inode_rejects_changed_fileid),
	KUNIT_CASE(update_inode_allows_mounted_on_fileid),
	KUNIT_CASE(update_inode_ignores_mounted_on_fileid_only),
	KUNIT_CASE(update_inode_rejects_changed_type),
	KUNIT_CASE(update_inode_accepts_matching_identity),
	{}
};

static struct kunit_suite nfs_update_inode_suite = {
	.name		= "nfs-inode-update",
	.test_cases	= nfs_update_inode_cases,
};

static struct kunit_case nfs_wcc_cases[] = {
	KUNIT_CASE(wcc_applies_change_attr_when_pre_matches),
	KUNIT_CASE(wcc_discards_change_attr_when_pre_differs),
	KUNIT_CASE(wcc_ignores_change_attr_without_prechange),
	KUNIT_CASE(wcc_change_on_directory_invalidates_data),
	KUNIT_CASE(wcc_applies_mtime_when_pre_matches),
	KUNIT_CASE(wcc_discards_mtime_when_pre_differs),
	KUNIT_CASE(wcc_applies_ctime_when_pre_matches),
	KUNIT_CASE(wcc_applies_size_when_pre_matches),
	KUNIT_CASE(wcc_discards_size_while_writebacks_pending),
	{}
};

static struct kunit_suite nfs_wcc_suite = {
	.name		= "nfs-inode-wcc",
	.test_cases	= nfs_wcc_cases,
};

static struct kunit_case nfs_refresh_cases[] = {
	KUNIT_CASE(refresh_inode_ignores_empty_fattr),
	KUNIT_CASE(refresh_inode_propagates_identity_rejection),
	{}
};

static struct kunit_suite nfs_refresh_suite = {
	.name		= "nfs-inode-refresh",
	.test_cases	= nfs_refresh_cases,
};

static struct kunit_case nfs_check_attrs_cases[] = {
	KUNIT_CASE(check_attrs_agreeing_flags_nothing),
	KUNIT_CASE(check_attrs_flags_changed_size),
	KUNIT_CASE(check_attrs_flags_changed_mtime),
	KUNIT_CASE(check_attrs_flags_changed_change_attr),
	KUNIT_CASE(check_attrs_compares_only_permission_bits),
	KUNIT_CASE(check_attrs_flags_changed_permissions),
	KUNIT_CASE(check_attrs_maps_owner_change_to_other),
	KUNIT_CASE(check_attrs_flags_changed_nlink),
	KUNIT_CASE(check_attrs_flags_changed_atime),
	KUNIT_CASE(check_attrs_rejects_changed_fileid),
	KUNIT_CASE(check_attrs_rejects_changed_type),
	KUNIT_CASE(check_attrs_skips_everything_when_delegated),
	{}
};

static struct kunit_suite nfs_check_attrs_suite = {
	.name		= "nfs-inode-check-attrs",
	.test_cases	= nfs_check_attrs_cases,
};

static struct kunit_case nfs_post_op_cases[] = {
	KUNIT_CASE(post_op_update_invalidates_directory_data),
	KUNIT_CASE(post_op_update_leaves_regular_file_data_alone),
	KUNIT_CASE(post_op_update_sets_a_barrier),
	{}
};

static struct kunit_suite nfs_post_op_suite = {
	.name		= "nfs-inode-post-op",
	.test_cases	= nfs_post_op_cases,
};

static struct kunit_case nfs_update_body_cases[] = {
	KUNIT_CASE(update_applies_mtime),
	KUNIT_CASE(update_applies_ctime_and_atime),
	KUNIT_CASE(update_applies_size_and_invalidates_data),
	KUNIT_CASE(update_does_not_shrink_file_with_writebacks),
	KUNIT_CASE(update_still_grows_file_with_writebacks),
	KUNIT_CASE(update_applies_only_permission_bits_of_mode),
	KUNIT_CASE(update_unchanged_mode_invalidates_nothing),
	KUNIT_CASE(update_applies_owner_and_invalidates_access),
	KUNIT_CASE(update_applies_nlink),
	KUNIT_CASE(update_converts_space_used_to_blocks),
	{}
};

static struct kunit_suite nfs_update_body_suite = {
	.name		= "nfs-inode-update-body",
	.test_cases	= nfs_update_body_cases,
};

static struct kunit_case nfs_setattr_cases[] = {
	KUNIT_CASE(setattr_update_applies_mode),
	KUNIT_CASE(setattr_update_applies_owner),
	KUNIT_CASE(setattr_update_invalidates_access_on_chown),
	KUNIT_CASE(setattr_update_sets_a_barrier),
	{}
};

static struct kunit_suite nfs_setattr_suite = {
	.name		= "nfs-inode-setattr",
	.test_cases	= nfs_setattr_cases,
};

static struct kunit_case nfs_timestamps_cases[] = {
	KUNIT_CASE(set_timestamps_applies_explicit_atime),
	KUNIT_CASE(set_timestamps_applies_explicit_mtime),
	KUNIT_CASE(set_timestamps_without_flags_changes_nothing),
	KUNIT_CASE(update_timestamps_mtime_also_clears_ctime),
	{}
};

static struct kunit_suite nfs_timestamps_suite = {
	.name		= "nfs-inode-timestamps",
	.test_cases	= nfs_timestamps_cases,
};

static struct kunit_case nfs_partial_cases[] = {
	KUNIT_CASE(ooo_record_captures_change_gap),
	KUNIT_CASE(ooo_record_ignores_incomplete_change_info),
	KUNIT_CASE(finish_partial_update_when_change_is_unmoved),
	KUNIT_CASE(finish_partial_update_declines_when_change_invalid),
	KUNIT_CASE(finish_partial_update_declines_when_cache_is_clean),
	{}
};

static struct kunit_suite nfs_partial_suite = {
	.name		= "nfs-inode-partial-update",
	.test_cases	= nfs_partial_cases,
};

static struct kunit_case nfs_find_actor_cases[] = {
	KUNIT_CASE(find_actor_matches_identical_inode),
	KUNIT_CASE(find_actor_rejects_different_fileid),
	KUNIT_CASE(find_actor_rejects_different_type),
	KUNIT_CASE(find_actor_rejects_different_filehandle),
	KUNIT_CASE(find_actor_rejects_stale_inode),
	KUNIT_CASE(init_locked_seeds_identity),
	KUNIT_CASE(init_locked_result_is_findable),
	{}
};

static struct kunit_suite nfs_find_actor_suite = {
	.name		= "nfs-inode-cache-match",
	.test_cases	= nfs_find_actor_cases,
};

static struct kunit_case nfs_readdirplus_cases[] = {
	KUNIT_CASE(readdirplus_enabled_when_all_conditions_hold),
	KUNIT_CASE(readdirplus_disabled_without_server_support),
	KUNIT_CASE(readdirplus_disabled_with_short_attr_timeout),
	{}
};

static struct kunit_suite nfs_readdirplus_suite = {
	.name		= "nfs-inode-readdirplus",
	.test_cases	= nfs_readdirplus_cases,
};

static struct kunit_case nfs_revalidate_cases[] = {
	KUNIT_CASE(revalidate_succeeds_and_queries_the_server),
	KUNIT_CASE(revalidate_stale_marks_regular_file_stale),
	KUNIT_CASE(revalidate_stale_only_zaps_directory),
	KUNIT_CASE(revalidate_timeout_is_tolerated_with_softreval),
	KUNIT_CASE(revalidate_timeout_propagates_without_softreval),
	KUNIT_CASE(revalidate_other_errors_propagate),
	KUNIT_CASE(revalidate_skips_already_stale_inode),
	{}
};

static struct kunit_suite nfs_revalidate_suite = {
	.name		= "nfs-inode-revalidate",
	.test_cases	= nfs_revalidate_cases,
};

static struct kunit_case nfs_revalidate_gate_cases[] = {
	KUNIT_CASE(revalidate_gate_skips_when_cache_is_valid),
	KUNIT_CASE(revalidate_gate_queries_when_flag_is_invalid),
	KUNIT_CASE(revalidate_gate_reports_stale_without_querying),
	KUNIT_CASE(mapping_needs_revalidate_when_change_is_invalid),
	KUNIT_CASE(mapping_needs_revalidate_when_stale),
	{}
};

static struct kunit_suite nfs_revalidate_gate_suite = {
	.name		= "nfs-inode-revalidate-gate",
	.test_cases	= nfs_revalidate_gate_cases,
};

static struct kunit_case nfs_sync_cases[] = {
	KUNIT_CASE(sync_inode_on_clean_regular_file_succeeds),
	KUNIT_CASE(sync_inode_on_clean_directory_succeeds),
	KUNIT_CASE(commit_inode_with_nothing_pending_succeeds),
	KUNIT_CASE(commit_inode_balances_its_counter),
	KUNIT_CASE(sync_mapping_without_pages_is_a_noop),
	{}
};

static struct kunit_suite nfs_sync_suite = {
	.name		= "nfs-inode-sync",
	.test_cases	= nfs_sync_cases,
};

static struct kunit_case nfs_lifetime_cases[] = {
	KUNIT_CASE(drop_inode_drops_stale_inode),
	KUNIT_CASE(drop_inode_drops_unlinked_inode),
	KUNIT_CASE(fixup_delegated_strips_times_under_mtime_delegation),
	KUNIT_CASE(fixup_delegated_keeps_times_it_admits_are_invalid),
	KUNIT_CASE(fixup_delegated_is_a_noop_without_delegation),
	KUNIT_CASE(buffered_writers_false_when_file_is_odirect),
	KUNIT_CASE(buffered_writers_false_without_open_files),
	{}
};

static struct kunit_suite nfs_lifetime_suite = {
	.name		= "nfs-inode-lifetime",
	.test_cases	= nfs_lifetime_cases,
};

static struct kunit_case nfs_lock_ctx_cases[] = {
	KUNIT_CASE(init_lock_context_starts_referenced_and_idle),
	KUNIT_CASE(find_lock_context_returns_null_when_empty),
	KUNIT_CASE(find_lock_context_matches_current_owner),
	KUNIT_CASE(find_lock_context_skips_other_owner),
	{}
};

static struct kunit_suite nfs_lock_ctx_suite = {
	.name		= "nfs-inode-lock-context",
	.test_cases	= nfs_lock_ctx_cases,
};

static struct kunit_case nfs_open_ctx_cases[] = {
	KUNIT_CASE(get_open_context_takes_a_reference),
	KUNIT_CASE(get_open_context_tolerates_null),
	KUNIT_CASE(get_open_context_refuses_dead_context),
	KUNIT_CASE(attach_open_context_links_it_to_the_inode),
	KUNIT_CASE(attach_open_context_invalidates_data_after_reordering),
	KUNIT_CASE(attach_open_context_leaves_clean_cache_alone),
	KUNIT_CASE(find_open_context_matches_mode_and_cred),
	KUNIT_CASE(find_open_context_requires_exact_mode),
	KUNIT_CASE(find_open_context_skips_closed_context),
	KUNIT_CASE(find_open_context_null_cred_matches_any),
	{}
};

static struct kunit_suite nfs_open_ctx_suite = {
	.name		= "nfs-inode-open-context",
	.test_cases	= nfs_open_ctx_cases,
};

static struct kunit_case nfs_ooo_test_cases[] = {
	KUNIT_CASE(ooo_test_false_when_nothing_outstanding),
	KUNIT_CASE(ooo_test_true_when_deferred),
	KUNIT_CASE(ooo_test_true_when_gaps_recorded),
	KUNIT_CASE(ooo_test_false_when_table_is_empty),
	KUNIT_CASE(clear_inode_drops_acl_validity),
	{}
};

static struct kunit_suite nfs_ooo_test_suite = {
	.name		= "nfs-inode-ooo-state",
	.test_cases	= nfs_ooo_test_cases,
};

kunit_test_suites(&nfs_attr_cmp_suite,
		  &nfs_cache_invalid_suite,
		  &nfs_cache_expiry_suite,
		  &nfs_ooo_suite,
		  &nfs_zap_suite,
		  &nfs_helper_suite,
		  &nfs_alloc_suite,
		  &nfs_update_inode_suite,
		  &nfs_wcc_suite,
		  &nfs_refresh_suite,
		  &nfs_check_attrs_suite,
		  &nfs_post_op_suite,
		  &nfs_update_body_suite,
		  &nfs_setattr_suite,
		  &nfs_timestamps_suite,
		  &nfs_partial_suite,
		  &nfs_find_actor_suite,
		  &nfs_readdirplus_suite,
		  &nfs_revalidate_suite,
		  &nfs_revalidate_gate_suite,
		  &nfs_sync_suite,
		  &nfs_lifetime_suite,
		  &nfs_lock_ctx_suite,
		  &nfs_open_ctx_suite,
		  &nfs_ooo_test_suite);

MODULE_DESCRIPTION("Test NFS inode attribute freshness comparison");
MODULE_LICENSE("GPL");
