// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the decision logic in fs/nfs/nfs4proc.c.
 *
 * nfs4proc.c is 11k lines and almost all of it issues RPCs, which is why
 * it has no unit tests. Underneath that, though, sit a handful of pure
 * functions that decide what the client asks for and how it reacts:
 * translating NFSv4 status codes to errnos, backing off between retries,
 * mapping open modes onto share-access bits, downgrading open claims for
 * servers that lack a feature, and trimming the GETATTR bitmask for
 * attributes a delegation already covers.
 *
 * Those decisions are invisible to xfstests -- a wrong share-access bit or
 * an over-broad attribute mask still produces correct file contents, just
 * a different conversation with the server. They are exactly what unit
 * tests are for.
 *
 * nfs4_bitmap_copy_adjust() reaches the delegation state through
 * NFS_PROTO(inode)->have_delegation(), which is a vtable, so a stub server
 * is enough to drive every branch.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/nfs.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>

#include "nfs4_fs.h"
#include "internal.h"
#include "nfs4session.h"
#include "delegation.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Private to nfs4proc.c; un-staticed by scripts/kunit/run-sunrpc-kunit.sh. */
int nfs4_map_errors(int err);
long nfs4_update_delay(long *timeout);
u32 nfs4_fmode_to_share_access(fmode_t fmode);
u32 nfs4_map_atomic_open_share(struct nfs_server *server, fmode_t fmode,
			       int openflags);
enum open_claim_type4 nfs4_map_atomic_open_claim(struct nfs_server *server,
						 enum open_claim_type4 claim);
const nfs4_stateid *nfs4_recoverable_stateid(const nfs4_stateid *stateid);
void nfs4_bitmap_copy_adjust(__u32 *dst, const __u32 *src,
			     struct inode *inode, unsigned long flags);
void nfs4_slot_sequence_record_sent(struct nfs4_slot *slot, u32 seqnr);
void nfs4_slot_sequence_acked(struct nfs4_slot *slot, u32 seqnr);
fmode_t _nfs4_ctx_to_accessmode(const struct nfs_open_context *ctx);
fmode_t _nfs4_ctx_to_openmode(const struct nfs_open_context *ctx);

/*
 * nfs4_map_errors(): NFSv4 status to errno
 *
 * Anything at or above -1000 is already an errno and passes straight
 * through; NFSv4 status codes start at 10000 and are translated, with
 * anything unrecognised collapsing to -EIO.
 */

struct map_error_param {
	const char	*desc;
	int		err;
	int		expected;
};

static void map_error_get_desc(const struct map_error_param *p, char *desc)
{
	strscpy(desc, p->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct map_error_param map_error_params[] = {
	/* Already errnos: returned unchanged. */
	{ "success passes through",		0,		0 },
	{ "EIO passes through",			-EIO,		-EIO },
	{ "EACCES passes through",		-EACCES,	-EACCES },
	/*
	 * The boundary of the pass-through test. -1000 is not a real errno,
	 * but it pins where the function stops trusting its input.
	 */
	{ "minus 1000 passes through",		-1000,		-1000 },
	{ "minus 1001 is not an errno",		-1001,		-EIO },

	/* Server is busy or conflicted: retryable at a higher layer. */
	{ "RESOURCE",		-NFS4ERR_RESOURCE,		-EREMOTEIO },
	{ "LAYOUTTRYLATER",	-NFS4ERR_LAYOUTTRYLATER,	-EREMOTEIO },
	{ "RECALLCONFLICT",	-NFS4ERR_RECALLCONFLICT,	-EREMOTEIO },
	{ "RETURNCONFLICT",	-NFS4ERR_RETURNCONFLICT,	-EREMOTEIO },

	/* Security failures are reported as permission problems. */
	{ "WRONGSEC",		-NFS4ERR_WRONGSEC,		-EPERM },
	{ "WRONG_CRED",		-NFS4ERR_WRONG_CRED,		-EPERM },

	/* Malformed owner or name strings. */
	{ "BADOWNER",		-NFS4ERR_BADOWNER,		-EINVAL },
	{ "BADNAME",		-NFS4ERR_BADNAME,		-EINVAL },

	{ "SHARE_DENIED",	-NFS4ERR_SHARE_DENIED,		-EACCES },
	{ "MINOR_VERS_MISMATCH", -NFS4ERR_MINOR_VERS_MISMATCH,	-EPROTONOSUPPORT },
	{ "FILE_OPEN",		-NFS4ERR_FILE_OPEN,		-EBUSY },
	{ "NOT_SAME",		-NFS4ERR_NOT_SAME,		-ENOTSYNC },

	/* Anything in the 10000 series the client does not model becomes EIO. */
	{ "unmapped status",	-NFS4ERR_BADXDR,		-EIO },
	/*
	 * NFSv4 status codes come in two ranges, and only one is translated
	 * here. NFS4ERR_STALE is 70, inside the pass-through range, so it is
	 * returned untouched: codes below 1000 have already been turned into
	 * errnos by nfs4_stat_to_errno() further down the stack, and only the
	 * 10000-series codes still need mapping.
	 */
	{ "low-numbered status passes through", -NFS4ERR_STALE, -NFS4ERR_STALE },
};

KUNIT_ARRAY_PARAM(map_error, map_error_params, map_error_get_desc);

static void map_error_case(struct kunit *test)
{
	const struct map_error_param *p = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, nfs4_map_errors(p->err), p->expected,
			    "mapping %d", p->err);
}

/* Whatever comes back is negative or zero -- never a positive value. */
static void map_error_never_returns_positive(struct kunit *test)
{
	const struct map_error_param *p = test->param_value;

	KUNIT_EXPECT_LE(test, nfs4_map_errors(p->err), 0);
}

/*
 * nfs4_update_delay(): exponential backoff between retries
 *
 * The bounds are file-private #defines in nfs4proc.c, so they are read
 * back out of the function rather than copied here: a NULL timeout yields
 * the maximum, and a zeroed one yields the minimum.
 */

static long retry_max(void)
{
	return nfs4_update_delay(NULL);
}

static long retry_min(void)
{
	long timeout = 0;

	return nfs4_update_delay(&timeout);
}

/* A caller with nowhere to store state always waits the longest interval. */
static void absent_timeout_returns_the_maximum(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_update_delay(NULL), retry_max());
}

/*
 * Pins the actual bounds, which the tests above deliberately do not. If
 * NFS4_POLL_RETRY_MIN or _MAX changes upstream this fails, which is the
 * point: the change should be noticed rather than absorbed.
 */
static void retry_bounds_are_a_tenth_of_a_second_to_fifteen(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, retry_min(), HZ / 10);
	KUNIT_EXPECT_EQ(test, retry_max(), 15 * HZ);
}

static void first_delay_starts_at_the_minimum(struct kunit *test)
{
	long timeout = 0;

	KUNIT_EXPECT_EQ(test, nfs4_update_delay(&timeout), retry_min());
}

/* A negative timeout is treated as unset rather than propagated. */
static void negative_timeout_is_reset_to_the_minimum(struct kunit *test)
{
	long timeout = -5000;

	KUNIT_EXPECT_EQ(test, nfs4_update_delay(&timeout), retry_min());
}

static void each_delay_doubles_the_last(struct kunit *test)
{
	long timeout = 0;
	long first = nfs4_update_delay(&timeout);
	long second = nfs4_update_delay(&timeout);
	long third = nfs4_update_delay(&timeout);

	KUNIT_EXPECT_EQ(test, second, first * 2);
	KUNIT_EXPECT_EQ(test, third, second * 2);
}

static void delay_is_capped_at_the_maximum(struct kunit *test)
{
	long timeout = retry_max() * 4;

	KUNIT_EXPECT_EQ_MSG(test, nfs4_update_delay(&timeout), retry_max(),
			    "an oversized timeout was not clamped");
}

/*
 * The clamp is applied on entry, not on exit, so the stored value is left
 * at twice the maximum and is only brought back into range by the next
 * call. Recorded because it looks like a bug and is not: every caller
 * passes the value straight back in.
 */
static void stored_timeout_exceeds_the_cap_until_the_next_call(struct kunit *test)
{
	long timeout = 0;
	int i;

	for (i = 0; i < 20; i++)
		nfs4_update_delay(&timeout);

	KUNIT_EXPECT_EQ(test, timeout, retry_max() * 2);
	KUNIT_EXPECT_EQ(test, nfs4_update_delay(&timeout), retry_max());
}

/* However long a caller keeps retrying, the wait never exceeds the cap. */
static void repeated_delays_never_exceed_the_cap(struct kunit *test)
{
	long timeout = 0;
	int i;

	for (i = 0; i < 50; i++)
		KUNIT_ASSERT_LE_MSG(test, nfs4_update_delay(&timeout),
				    retry_max(), "iteration %d exceeded the cap",
				    i);
}

/*
 * Open modes to NFSv4 share access
 */

static void fmode_maps_to_share_access(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_fmode_to_share_access(FMODE_READ),
			NFS4_SHARE_ACCESS_READ);
	KUNIT_EXPECT_EQ(test, nfs4_fmode_to_share_access(FMODE_WRITE),
			NFS4_SHARE_ACCESS_WRITE);
	KUNIT_EXPECT_EQ(test, nfs4_fmode_to_share_access(FMODE_READ | FMODE_WRITE),
			NFS4_SHARE_ACCESS_BOTH);
}

/*
 * Only the read and write bits are considered. An exec-only mode carries
 * no share access of its own -- _nfs4_ctx_to_openmode() is what turns exec
 * into a read.
 */
static void fmode_without_read_or_write_has_no_share_access(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_fmode_to_share_access(0), 0);
	KUNIT_EXPECT_EQ_MSG(test, nfs4_fmode_to_share_access(FMODE_EXEC), 0,
			    "exec alone claimed share access");
}

/* The access mode keeps exec; the open mode converts it to a read. */
static void exec_becomes_read_in_the_open_mode_only(struct kunit *test)
{
	struct nfs_open_context ctx = { .mode = FMODE_EXEC };

	KUNIT_EXPECT_EQ(test, _nfs4_ctx_to_accessmode(&ctx), FMODE_EXEC);
	KUNIT_EXPECT_EQ(test, _nfs4_ctx_to_openmode(&ctx), FMODE_READ);
}

static void open_mode_keeps_read_and_write_alongside_exec(struct kunit *test)
{
	struct nfs_open_context ctx = { .mode = FMODE_EXEC | FMODE_WRITE };

	KUNIT_EXPECT_EQ(test, _nfs4_ctx_to_openmode(&ctx),
			FMODE_READ | FMODE_WRITE);
}

/* Modes outside the read/write/exec set are not carried into the request. */
static void unrelated_mode_bits_are_dropped(struct kunit *test)
{
	struct nfs_open_context ctx = { .mode = FMODE_READ | FMODE_LSEEK };

	KUNIT_EXPECT_EQ(test, _nfs4_ctx_to_accessmode(&ctx), FMODE_READ);
	KUNIT_EXPECT_EQ(test, _nfs4_ctx_to_openmode(&ctx), FMODE_READ);
}

/*
 * nfs4_map_atomic_open_share(): the delegation hints attached to an OPEN
 */

static struct nfs_server *server_with_caps(struct kunit *test, u32 caps)
{
	struct nfs_server *server;

	server = kunit_kzalloc(test, sizeof(*server), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, server);
	server->caps = caps;
	return server;
}

/* A server without the v1 open capability gets bare share access. */
static void without_atomic_open_v1_no_hints_are_sent(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test, 0);

	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_share(server, FMODE_READ, O_DIRECT),
			NFS4_SHARE_ACCESS_READ);
}

/*
 * O_DIRECT bypasses the page cache, so a delegation would buy nothing:
 * the client asks for none, and asks for nothing else either.
 */
static void o_direct_asks_for_no_delegation(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test,
			NFS_CAP_ATOMIC_OPEN_V1 | NFS_CAP_DELEGTIME |
			NFS_CAP_OPEN_XOR);
	u32 res = nfs4_map_atomic_open_share(server, FMODE_WRITE, O_DIRECT);

	KUNIT_EXPECT_EQ(test, res,
			NFS4_SHARE_ACCESS_WRITE | NFS4_SHARE_WANT_NO_DELEG);
	KUNIT_EXPECT_FALSE_MSG(test, res & NFS4_SHARE_WANT_DELEG_TIMESTAMPS,
			       "asked for delegated timestamps under O_DIRECT");
}

static void delegtime_capability_requests_timestamps(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test,
			NFS_CAP_ATOMIC_OPEN_V1 | NFS_CAP_DELEGTIME);

	KUNIT_EXPECT_EQ(test, nfs4_map_atomic_open_share(server, FMODE_READ, 0),
			NFS4_SHARE_ACCESS_READ |
			NFS4_SHARE_WANT_DELEG_TIMESTAMPS);
}

static void open_xor_capability_requests_xor_delegation(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test,
			NFS_CAP_ATOMIC_OPEN_V1 | NFS_CAP_OPEN_XOR);

	KUNIT_EXPECT_EQ(test, nfs4_map_atomic_open_share(server, FMODE_READ, 0),
			NFS4_SHARE_ACCESS_READ |
			NFS4_SHARE_WANT_OPEN_XOR_DELEGATION);
}

static void capabilities_combine(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test,
			NFS_CAP_ATOMIC_OPEN_V1 | NFS_CAP_DELEGTIME |
			NFS_CAP_OPEN_XOR);

	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_share(server,
						   FMODE_READ | FMODE_WRITE, 0),
			NFS4_SHARE_ACCESS_BOTH |
			NFS4_SHARE_WANT_DELEG_TIMESTAMPS |
			NFS4_SHARE_WANT_OPEN_XOR_DELEGATION);
}

/*
 * nfs4_map_atomic_open_claim(): downgrading claims for older servers
 *
 * The filehandle-based claim types were added with the v1 atomic open. A
 * server without it is sent the name-based equivalent instead.
 */

static void v1_server_receives_every_claim_unchanged(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test,
						     NFS_CAP_ATOMIC_OPEN_V1);

	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server, NFS4_OPEN_CLAIM_FH),
			NFS4_OPEN_CLAIM_FH);
	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server,
						   NFS4_OPEN_CLAIM_DELEG_CUR_FH),
			NFS4_OPEN_CLAIM_DELEG_CUR_FH);
}

static void older_server_receives_downgraded_claims(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test, 0);

	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server, NFS4_OPEN_CLAIM_FH),
			NFS4_OPEN_CLAIM_NULL);
	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server,
						   NFS4_OPEN_CLAIM_DELEG_CUR_FH),
			NFS4_OPEN_CLAIM_DELEGATE_CUR);
	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server,
						   NFS4_OPEN_CLAIM_DELEG_PREV_FH),
			NFS4_OPEN_CLAIM_DELEGATE_PREV);
}

/* Claims with no filehandle variant are left alone on any server. */
static void claims_without_a_filehandle_form_are_unchanged(struct kunit *test)
{
	struct nfs_server *server = server_with_caps(test, 0);

	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server, NFS4_OPEN_CLAIM_NULL),
			NFS4_OPEN_CLAIM_NULL);
	KUNIT_EXPECT_EQ(test,
			nfs4_map_atomic_open_claim(server,
						   NFS4_OPEN_CLAIM_PREVIOUS),
			NFS4_OPEN_CLAIM_PREVIOUS);
}

/*
 * nfs4_recoverable_stateid(): which stateids survive a server restart
 */

static nfs4_stateid *stateid_of_type(struct kunit *test, int type)
{
	nfs4_stateid *sid = kunit_kzalloc(test, sizeof(*sid), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sid);
	sid->type = type;
	return sid;
}

static void open_lock_and_delegation_stateids_are_recoverable(struct kunit *test)
{
	nfs4_stateid *open = stateid_of_type(test, NFS4_OPEN_STATEID_TYPE);
	nfs4_stateid *lock = stateid_of_type(test, NFS4_LOCK_STATEID_TYPE);
	nfs4_stateid *deleg = stateid_of_type(test, NFS4_DELEGATION_STATEID_TYPE);

	KUNIT_EXPECT_PTR_EQ(test, nfs4_recoverable_stateid(open), open);
	KUNIT_EXPECT_PTR_EQ(test, nfs4_recoverable_stateid(lock), lock);
	KUNIT_EXPECT_PTR_EQ(test, nfs4_recoverable_stateid(deleg), deleg);
}

static void other_stateids_are_not_recoverable(struct kunit *test)
{
	nfs4_stateid *invalid = stateid_of_type(test, NFS4_INVALID_STATEID_TYPE);
	nfs4_stateid *special = stateid_of_type(test, NFS4_SPECIAL_STATEID_TYPE);

	KUNIT_EXPECT_PTR_EQ(test, nfs4_recoverable_stateid(invalid), NULL);
	KUNIT_EXPECT_PTR_EQ_MSG(test, nfs4_recoverable_stateid(special), NULL,
				"a special stateid was treated as recoverable");
}

static void absent_stateid_is_not_recoverable(struct kunit *test)
{
	KUNIT_EXPECT_PTR_EQ(test, nfs4_recoverable_stateid(NULL), NULL);
}

/*
 * Slot sequence tracking
 *
 * Sequence numbers are u32 and wrap. The comparison is done on the signed
 * difference so that wrapping still reads as "later", which a plain
 * seqnr > highest would get wrong exactly once per cycle.
 */

static void recording_a_later_sequence_advances_the_high_mark(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 5 };

	nfs4_slot_sequence_record_sent(&slot, 9);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_highest_sent, 9U);
}

static void recording_an_earlier_sequence_does_not_move_it(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 9 };

	nfs4_slot_sequence_record_sent(&slot, 5);
	KUNIT_EXPECT_EQ_MSG(test, slot.seq_nr_highest_sent, 9U,
			    "a retransmission moved the high mark backwards");
}

static void recording_the_same_sequence_does_not_move_it(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 9 };

	nfs4_slot_sequence_record_sent(&slot, 9);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_highest_sent, 9U);
}

/* Wrapping past U32_MAX still counts as moving forwards. */
static void sequence_numbers_wrap_forwards(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = U32_MAX };

	nfs4_slot_sequence_record_sent(&slot, 1);
	KUNIT_EXPECT_EQ_MSG(test, slot.seq_nr_highest_sent, 1U,
			    "a wrapped sequence number was read as older");
}

/* ...and a genuinely old number near the wrap is still rejected. */
static void a_stale_sequence_near_the_wrap_is_rejected(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 1 };

	nfs4_slot_sequence_record_sent(&slot, U32_MAX);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_highest_sent, 1U);
}

/* Acking records the send as well, so an ack can never lag a send. */
static void acking_also_records_the_send(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 2 };

	nfs4_slot_sequence_acked(&slot, 7);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_last_acked, 7U);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_highest_sent, 7U);
}

/*
 * Unlike the high-water mark, the last-acked value is assigned outright,
 * so a late reply to an older request moves it backwards.
 */
static void acking_an_older_sequence_still_assigns_it(struct kunit *test)
{
	struct nfs4_slot slot = { .seq_nr_highest_sent = 9,
				  .seq_nr_last_acked = 9 };

	nfs4_slot_sequence_acked(&slot, 4);
	KUNIT_EXPECT_EQ(test, slot.seq_nr_last_acked, 4U);
	KUNIT_EXPECT_EQ_MSG(test, slot.seq_nr_highest_sent, 9U,
			    "a late ack dragged the high mark backwards");
}

/*
 * nfs4_bitmap_copy_adjust(): trimming GETATTR under a delegation
 *
 * Holding a delegation means the client owns some attributes outright, so
 * asking the server for them again is wasted work -- unless the cache has
 * been invalidated for that attribute, in which case it must be fetched.
 *
 * The delegation state is reached through NFS_PROTO(inode)->have_delegation,
 * which takes (inode, fmode, flags): FMODE_READ with no flags asks "any
 * delegation", and NFS_DELEGATION_FLAG_TIME with FMODE_READ or FMODE_WRITE
 * asks about delegated atime or mtime respectively.
 */

struct deleg_stub {
	bool	any;
	bool	atime;
	bool	mtime;
};

struct bitmap_fixture {
	struct nfs_inode	nfsi;
	struct nfs_server	server;
	struct nfs_client	client;
	struct super_block	sb;
	struct nfs_rpc_ops	rpc_ops;
	struct deleg_stub	deleg;
};

static struct bitmap_fixture *cur_fixture;

static int stub_have_delegation(struct inode *inode, fmode_t type, int flags)
{
	if (flags & NFS_DELEGATION_FLAG_TIME)
		return (type & FMODE_WRITE) ? cur_fixture->deleg.mtime
					    : cur_fixture->deleg.atime;
	return cur_fixture->deleg.any;
}

static struct inode *bitmap_inode(struct kunit *test, bool any, bool atime,
				  bool mtime, unsigned long cache_validity)
{
	struct bitmap_fixture *f;
	struct inode *inode;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->deleg.any = any;
	f->deleg.atime = atime;
	f->deleg.mtime = mtime;

	/* NFS_PROTO(inode) is NFS_SERVER(inode)->nfs_client->rpc_ops. */
	f->rpc_ops.have_delegation = stub_have_delegation;
	f->client.rpc_ops = &f->rpc_ops;
	f->server.nfs_client = &f->client;
	f->sb.s_fs_info = &f->server;

	inode = &f->nfsi.vfs_inode;
	inode->i_sb = &f->sb;
	f->nfsi.cache_validity = cache_validity;

	cur_fixture = f;
	return inode;
}

/*
 * A full mask, so every clearing branch is visible as a change.
 *
 * nfs4proc.c sizes these with its own NFS4_BITMASK_SZ, a file-private
 * define; NFS_BITMASK_SZ from nfs_xdr.h is the same value and is the one
 * a test can actually see.
 */
static const __u32 full_mask[NFS_BITMASK_SZ] = {
	FATTR4_WORD0_SIZE | FATTR4_WORD0_CHANGE,
	FATTR4_WORD1_RAWDEV | FATTR4_WORD1_MODE | FATTR4_WORD1_OWNER |
		FATTR4_WORD1_OWNER_GROUP | FATTR4_WORD1_TIME_ACCESS |
		FATTR4_WORD1_TIME_MODIFY | FATTR4_WORD1_TIME_METADATA,
	0,
};

/* With no inode there is nothing to know about, so the mask is copied. */
static void absent_inode_copies_the_mask_unchanged(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];

	nfs4_bitmap_copy_adjust(dst, full_mask, NULL, 0);

	KUNIT_EXPECT_EQ(test, dst[0], full_mask[0]);
	KUNIT_EXPECT_EQ(test, dst[1], full_mask[1]);
}

/* Without a delegation the client owns nothing and must ask for it all. */
static void undelegated_inode_copies_the_mask_unchanged(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, false, false, false, 0);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_EQ(test, dst[0], full_mask[0]);
	KUNIT_EXPECT_EQ_MSG(test, dst[1], full_mask[1],
			    "trimmed the mask without holding a delegation");
}

/*
 * Under a delegation the client controls the file, so a valid cache means
 * size, change, mode and ownership need not be re-fetched. rawdev is
 * always dropped: it cannot change for an open file.
 */
static void delegation_with_a_valid_cache_trims_owned_attributes(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, false, false, 0);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_FALSE(test, dst[0] & FATTR4_WORD0_SIZE);
	KUNIT_EXPECT_FALSE(test, dst[0] & FATTR4_WORD0_CHANGE);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_MODE);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_OWNER);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_OWNER_GROUP);
	KUNIT_EXPECT_FALSE_MSG(test, dst[1] & FATTR4_WORD1_RAWDEV,
			       "rawdev is requested even under a delegation");
}

/* An invalidated attribute is asked for again despite the delegation. */
static void invalid_size_is_still_requested(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, false, false,
					   NFS_INO_INVALID_SIZE);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_TRUE_MSG(test, dst[0] & FATTR4_WORD0_SIZE,
			      "dropped size from the mask while it was invalid");
	KUNIT_EXPECT_FALSE(test, dst[0] & FATTR4_WORD0_CHANGE);
}

/* The flags argument is folded in as if it were extra cache invalidation. */
static void flags_argument_acts_as_extra_invalidation(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, false, false, 0);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, NFS_INO_INVALID_CHANGE);

	KUNIT_EXPECT_TRUE_MSG(test, dst[0] & FATTR4_WORD0_CHANGE,
			      "the flags argument did not force a re-fetch");
}

/* A delegation over mtime lets all three timestamps be dropped. */
static void delegated_mtime_trims_every_timestamp(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, false, true, 0);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_TIME_ACCESS);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_TIME_MODIFY);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_TIME_METADATA);
}

/*
 * A delegation over atime alone covers only the access time; modify and
 * metadata times still have to be fetched.
 */
static void delegated_atime_trims_only_the_access_time(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, true, false, 0);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_TIME_ACCESS);
	KUNIT_EXPECT_TRUE_MSG(test, dst[1] & FATTR4_WORD1_TIME_MODIFY,
			      "an atime delegation dropped the modify time");
	KUNIT_EXPECT_TRUE_MSG(test, dst[1] & FATTR4_WORD1_TIME_METADATA,
			      "an atime delegation dropped the metadata time");
}

/* An invalidated atime is re-fetched even under a timestamp delegation. */
static void invalid_atime_is_still_requested(struct kunit *test)
{
	__u32 dst[NFS_BITMASK_SZ];
	struct inode *inode = bitmap_inode(test, true, false, true,
					   NFS_INO_INVALID_ATIME);

	nfs4_bitmap_copy_adjust(dst, full_mask, inode, 0);

	KUNIT_EXPECT_TRUE(test, dst[1] & FATTR4_WORD1_TIME_ACCESS);
	KUNIT_EXPECT_FALSE(test, dst[1] & FATTR4_WORD1_TIME_MODIFY);
}

/*
 * Suites
 */

static struct kunit_case nfs4_map_errors_cases[] = {
	{
		.name			= "map status",
		.run_case		= map_error_case,
		.generate_params	= map_error_gen_params,
	},
	{
		.name			= "never positive",
		.run_case		= map_error_never_returns_positive,
		.generate_params	= map_error_gen_params,
	},
	{}
};

static struct kunit_suite nfs4_map_errors_suite = {
	.name		= "nfs4-map-errors",
	.test_cases	= nfs4_map_errors_cases,
};

static struct kunit_case nfs4_delay_cases[] = {
	KUNIT_CASE(absent_timeout_returns_the_maximum),
	KUNIT_CASE(retry_bounds_are_a_tenth_of_a_second_to_fifteen),
	KUNIT_CASE(first_delay_starts_at_the_minimum),
	KUNIT_CASE(negative_timeout_is_reset_to_the_minimum),
	KUNIT_CASE(each_delay_doubles_the_last),
	KUNIT_CASE(delay_is_capped_at_the_maximum),
	KUNIT_CASE(stored_timeout_exceeds_the_cap_until_the_next_call),
	KUNIT_CASE(repeated_delays_never_exceed_the_cap),
	{}
};

static struct kunit_suite nfs4_delay_suite = {
	.name		= "nfs4-retry-delay",
	.test_cases	= nfs4_delay_cases,
};

static struct kunit_case nfs4_share_access_cases[] = {
	KUNIT_CASE(fmode_maps_to_share_access),
	KUNIT_CASE(fmode_without_read_or_write_has_no_share_access),
	KUNIT_CASE(exec_becomes_read_in_the_open_mode_only),
	KUNIT_CASE(open_mode_keeps_read_and_write_alongside_exec),
	KUNIT_CASE(unrelated_mode_bits_are_dropped),
	KUNIT_CASE(without_atomic_open_v1_no_hints_are_sent),
	KUNIT_CASE(o_direct_asks_for_no_delegation),
	KUNIT_CASE(delegtime_capability_requests_timestamps),
	KUNIT_CASE(open_xor_capability_requests_xor_delegation),
	KUNIT_CASE(capabilities_combine),
	{}
};

static struct kunit_suite nfs4_share_access_suite = {
	.name		= "nfs4-share-access",
	.test_cases	= nfs4_share_access_cases,
};

static struct kunit_case nfs4_open_claim_cases[] = {
	KUNIT_CASE(v1_server_receives_every_claim_unchanged),
	KUNIT_CASE(older_server_receives_downgraded_claims),
	KUNIT_CASE(claims_without_a_filehandle_form_are_unchanged),
	{}
};

static struct kunit_suite nfs4_open_claim_suite = {
	.name		= "nfs4-open-claim",
	.test_cases	= nfs4_open_claim_cases,
};

static struct kunit_case nfs4_stateid_cases[] = {
	KUNIT_CASE(open_lock_and_delegation_stateids_are_recoverable),
	KUNIT_CASE(other_stateids_are_not_recoverable),
	KUNIT_CASE(absent_stateid_is_not_recoverable),
	{}
};

static struct kunit_suite nfs4_stateid_suite = {
	.name		= "nfs4-stateid-recoverable",
	.test_cases	= nfs4_stateid_cases,
};

static struct kunit_case nfs4_slot_sequence_cases[] = {
	KUNIT_CASE(recording_a_later_sequence_advances_the_high_mark),
	KUNIT_CASE(recording_an_earlier_sequence_does_not_move_it),
	KUNIT_CASE(recording_the_same_sequence_does_not_move_it),
	KUNIT_CASE(sequence_numbers_wrap_forwards),
	KUNIT_CASE(a_stale_sequence_near_the_wrap_is_rejected),
	KUNIT_CASE(acking_also_records_the_send),
	KUNIT_CASE(acking_an_older_sequence_still_assigns_it),
	{}
};

static struct kunit_suite nfs4_slot_sequence_suite = {
	.name		= "nfs4-slot-sequence",
	.test_cases	= nfs4_slot_sequence_cases,
};

static struct kunit_case nfs4_bitmap_cases[] = {
	KUNIT_CASE(absent_inode_copies_the_mask_unchanged),
	KUNIT_CASE(undelegated_inode_copies_the_mask_unchanged),
	KUNIT_CASE(delegation_with_a_valid_cache_trims_owned_attributes),
	KUNIT_CASE(invalid_size_is_still_requested),
	KUNIT_CASE(flags_argument_acts_as_extra_invalidation),
	KUNIT_CASE(delegated_mtime_trims_every_timestamp),
	KUNIT_CASE(delegated_atime_trims_only_the_access_time),
	KUNIT_CASE(invalid_atime_is_still_requested),
	{}
};

static struct kunit_suite nfs4_bitmap_suite = {
	.name		= "nfs4-getattr-bitmap",
	.test_cases	= nfs4_bitmap_cases,
};

kunit_test_suites(&nfs4_map_errors_suite,
		  &nfs4_delay_suite,
		  &nfs4_share_access_suite,
		  &nfs4_open_claim_suite,
		  &nfs4_stateid_suite,
		  &nfs4_slot_sequence_suite,
		  &nfs4_bitmap_suite);

MODULE_DESCRIPTION("Test NFSv4 protocol decision logic");
MODULE_LICENSE("GPL");
