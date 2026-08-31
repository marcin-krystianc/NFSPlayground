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
#include <linux/iversion.h>
#include <linux/sunrpc/sched.h>

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
bool nfs4_server_supports_acls(const struct nfs_server *server,
			       enum nfs4_acl_type type);
ssize_t nfs4_proc_get_acl(struct inode *inode, void *buf, size_t buflen,
			  enum nfs4_acl_type type);
int nfs4_proc_set_acl(struct inode *inode, const void *buf, size_t buflen,
		      enum nfs4_acl_type type);
int __nfs4_proc_set_acl(struct inode *inode, const void *buf, size_t buflen,
			enum nfs4_acl_type type);
void nfs4_update_changeattr_locked(struct inode *inode,
				   struct nfs4_change_info *cinfo,
				   unsigned long timestamp,
				   unsigned long cache_validity);
int nfs4_proc_unlink_done(struct rpc_task *task, struct inode *dir);
void nfs4_fattr_set_prechange(struct nfs_fattr *fattr, u64 version);
int nfs4_exception_should_retrans(const struct nfs_server *server,
				  struct nfs4_exception *exception);
bool _nfs4_is_integrity_protected(struct nfs_client *clp);
int nfs4_sequence_process(struct rpc_task *task, struct nfs4_sequence_res *res);
void nfs4_sequence_free_slot(struct nfs4_sequence_res *res);
bool nfs4_clear_cap_atomic_open_v1(struct nfs_server *server, int err,
				   struct nfs4_exception *exception);
bool nfs4_mode_match_open_stateid(struct nfs4_state *state, fmode_t fmode);
int can_open_cached(struct nfs4_state *state, fmode_t mode, int open_mode,
		    enum open_claim_type4 claim);
int can_open_delegated(struct nfs_delegation *delegation, fmode_t fmode,
		       enum open_claim_type4 claim);
void update_open_stateflags(struct nfs4_state *state, fmode_t fmode);
bool nfs_open_stateid_recover_openmode(struct nfs4_state *state);
void nfs_state_log_update_open_stateid(struct nfs4_state *state);
void nfs_resync_open_stateid_locked(struct nfs4_state *state);
void nfs_clear_open_stateid_locked(struct nfs4_state *state,
				   nfs4_stateid *stateid, fmode_t fmode);
void nfs_state_clear_open_state_flags(struct nfs4_state *state);
void nfs_state_set_delegation(struct nfs4_state *state,
			      const nfs4_stateid *deleg_stateid, fmode_t fmode);
void nfs_state_clear_delegation(struct nfs4_state *state);
int nfs4_check_cl_exchange_flags(u32 flags, u32 version);
bool nfs41_same_server_scope(struct nfs41_server_scope *a,
			     struct nfs41_server_scope *b);
int nfs4_verify_fore_channel_attrs(struct nfs41_create_session_args *args,
				   struct nfs41_create_session_res *res);
int nfs4_verify_back_channel_attrs(struct nfs41_create_session_args *args,
				   struct nfs41_create_session_res *res);
int nfs4_verify_channel_attrs(struct nfs41_create_session_args *args,
			      struct nfs41_create_session_res *res);
bool nfs4_match_stateid(const nfs4_stateid *s1, const nfs4_stateid *s2);
bool nfs41_match_stateid(const nfs4_stateid *s1, const nfs4_stateid *s2);
bool nfs4_error_stateid_expired(int err);
bool _is_same_nfs4_pathname(struct nfs4_pathname *path1,
			    struct nfs4_pathname *path2);
void nfs4_sequence_attach_slot(struct nfs4_sequence_args *args,
			       struct nfs4_sequence_res *res,
			       struct nfs4_slot *slot);
void nfs4_inc_nlink_locked(struct inode *inode);
void nfs4_inc_nlink(struct inode *inode);
void nfs4_dec_nlink_locked(struct inode *inode);
void nfs4_update_changeattr(struct inode *dir, struct nfs4_change_info *cinfo,
			    unsigned long timestamp, unsigned long cache_validity);
bool nfs_stateid_is_sequential(struct nfs4_state *state,
			       const nfs4_stateid *stateid);
void nfs_clear_open_stateid(struct nfs4_state *state,
			    nfs4_stateid *arg_stateid, nfs4_stateid *stateid,
			    fmode_t fmode);
void nfs4_return_incompatible_delegation(struct inode *inode, fmode_t fmode);
void nfs4_close_context(struct nfs_open_context *ctx, int is_sync);
bool nfs4_read_plus_not_supported(struct rpc_task *task,
				  struct nfs_pgio_header *hdr);
bool nfs4_write_need_cache_consistency_data(struct nfs_pgio_header *hdr);
void nfs4_bitmask_set(__u32 bitmask[], const __u32 src[], struct inode *inode,
		      unsigned long cache_validity);
int nfs4_buf_to_pages_noslab(const void *buf, size_t buflen,
			     struct page **pages);
void nfs4_zap_acl_attr(struct inode *inode);
void nfs_fixup_secinfo_attributes(struct nfs_fattr *fattr);
void nfs4_disable_swap(struct inode *inode);
void nfs4_init_boot_verifier(const struct nfs_client *clp,
			     nfs4_verifier *bootverf);
void do_renew_lease(struct nfs_client *clp, unsigned long timestamp);
void renew_lease(const struct nfs_server *server, unsigned long timestamp);
extern short nfs_delay_retrans;

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
	struct address_space	mapping;
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
 * ACL entry points: the checks made before any RPC is sent
 *
 * These functions do issue RPCs, which is why nothing in nfs4proc.c's
 * request-sending half is normally reachable from a unit test. But each
 * one validates its arguments first and can refuse outright, and those
 * refusals are ordinary logic -- the kernel never gets as far as building
 * a task. Driving only those paths tests real protocol behaviour: the ACL
 * size ceiling, the rule that an ACL cannot be deleted, and what happens
 * on a filehandle the server never gave us.
 *
 * The tests stop short of the dispatch in every case. That is a real
 * limit, not an oversight: past these gates the next statement needs a
 * server.
 */

struct acl_fixture {
	struct nfs_inode	nfsi;
	struct nfs_server	server;
	struct super_block	sb;
};

/*
 * @fh_size of 0 models an inode whose filehandle was never filled in.
 * @acl_bit is the server's advertised support, from attr_bitmask.
 */
static struct inode *acl_inode(struct kunit *test, unsigned short fh_size,
			       u32 word0, u32 word1)
{
	struct acl_fixture *f;
	struct inode *inode;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->server.attr_bitmask[0] = word0;
	f->server.attr_bitmask[1] = word1;
	f->sb.s_fs_info = &f->server;

	inode = &f->nfsi.vfs_inode;
	inode->i_sb = &f->sb;
	f->nfsi.fh.size = fh_size;
	return inode;
}

/* What the server advertises decides which ACL flavours are available. */
static void acl_support_follows_the_attribute_bitmask(struct kunit *test)
{
	struct inode *acl = acl_inode(test, 4, FATTR4_WORD0_ACL, 0);
	struct inode *dacl = acl_inode(test, 4, 0, FATTR4_WORD1_DACL);
	struct inode *sacl = acl_inode(test, 4, 0, FATTR4_WORD1_SACL);

	KUNIT_EXPECT_TRUE(test,
			  nfs4_server_supports_acls(NFS_SERVER(acl), NFS4ACL_ACL));
	KUNIT_EXPECT_TRUE(test,
			  nfs4_server_supports_acls(NFS_SERVER(dacl), NFS4ACL_DACL));
	KUNIT_EXPECT_TRUE(test,
			  nfs4_server_supports_acls(NFS_SERVER(sacl), NFS4ACL_SACL));
}

/*
 * The flavours are tracked in different bitmask words, so support for one
 * must not imply support for another.
 */
static void acl_flavours_are_advertised_independently(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, FATTR4_WORD0_ACL, 0);
	struct nfs_server *server = NFS_SERVER(inode);

	KUNIT_EXPECT_FALSE_MSG(test,
			       nfs4_server_supports_acls(server, NFS4ACL_DACL),
			       "plain ACL support implied DACL support");
	KUNIT_EXPECT_FALSE_MSG(test,
			       nfs4_server_supports_acls(server, NFS4ACL_SACL),
			       "plain ACL support implied SACL support");
}

/*
 * An inode with a zero-length filehandle has no server-side object behind
 * it, so there is nothing to fetch an ACL from. Reported as "no such
 * attribute" rather than an I/O error.
 */
static void get_acl_on_an_empty_filehandle_reports_no_data(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 0, FATTR4_WORD0_ACL, 0);
	char buf[16];

	KUNIT_EXPECT_EQ(test,
			nfs4_proc_get_acl(inode, buf, sizeof(buf), NFS4ACL_ACL),
			-ENODATA);
}

static void set_acl_on_an_empty_filehandle_reports_no_data(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 0, FATTR4_WORD0_ACL, 0);
	char buf[16] = { 0 };

	KUNIT_EXPECT_EQ(test,
			nfs4_proc_set_acl(inode, buf, sizeof(buf), NFS4ACL_ACL),
			-ENODATA);
}

/*
 * The filehandle check comes first: an inode with no filehandle reports
 * ENODATA even for a flavour the server does not support, which would
 * otherwise be EOPNOTSUPP.
 */
static void the_filehandle_check_precedes_the_support_check(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 0, 0, 0);
	char buf[16];

	KUNIT_EXPECT_EQ_MSG(test,
			    nfs4_proc_get_acl(inode, buf, sizeof(buf),
					      NFS4ACL_ACL),
			    -ENODATA,
			    "the support check ran before the filehandle check");
}

/* A server that does not advertise the flavour is refused before dispatch. */
static void get_acl_for_an_unsupported_flavour_is_refused(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, 0, 0);
	char buf[16];

	KUNIT_EXPECT_EQ(test,
			nfs4_proc_get_acl(inode, buf, sizeof(buf), NFS4ACL_ACL),
			-EOPNOTSUPP);
}

static void set_acl_for_an_unsupported_flavour_is_refused(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, 0, 0);
	char buf[16] = { 0 };

	KUNIT_EXPECT_EQ(test,
			__nfs4_proc_set_acl(inode, buf, sizeof(buf), NFS4ACL_ACL),
			-EOPNOTSUPP);
}

/*
 * system.nfs4_acl cannot be removed, only replaced, so an empty value is
 * rejected rather than sent as a zero-length SETACL.
 */
static void setting_an_empty_acl_is_rejected(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, FATTR4_WORD0_ACL, 0);

	KUNIT_EXPECT_EQ_MSG(test,
			    __nfs4_proc_set_acl(inode, "", 0, NFS4ACL_ACL),
			    -EINVAL,
			    "an empty ACL was accepted for transmission");
}

/*
 * The emptiness check comes before the support check, so a zero-length
 * value is EINVAL even where the flavour is unsupported.
 */
static void the_empty_check_precedes_the_support_check(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, 0, 0);

	KUNIT_EXPECT_EQ(test, __nfs4_proc_set_acl(inode, "", 0, NFS4ACL_ACL),
			-EINVAL);
}

/*
 * The request is built into a fixed on-stack page array, so an ACL needing
 * more pages than that array holds is refused rather than overflowing it.
 * The ceiling is XATTR_SIZE_MAX worth of pages.
 */
static void an_oversized_acl_is_refused(struct kunit *test)
{
	struct inode *inode = acl_inode(test, 4, FATTR4_WORD0_ACL, 0);
	const size_t max_pages = DIV_ROUND_UP(XATTR_SIZE_MAX, PAGE_SIZE);
	size_t too_big = (max_pages + 1) << PAGE_SHIFT;

	KUNIT_EXPECT_EQ_MSG(test,
			    __nfs4_proc_set_acl(inode, NULL, too_big, NFS4ACL_ACL),
			    -ERANGE,
			    "an ACL larger than the page array was accepted");
}

/*
 * Processing a server reply
 *
 * Everything above stops before an RPC is sent. This section starts on the
 * other side of one: the reply has arrived, and the client has to decide
 * what its caches still mean. No transport is needed for that -- the
 * server is a struct nfs4_change_info the test fills in, exactly as the
 * XDR layer would have.
 *
 * The decision under test is NFSv4's directory-caching optimisation.
 * Every operation that modifies a directory returns change_info4:
 * the change attribute before and after, plus an "atomic" flag saying the
 * server held the directory still across the operation. When the client
 * made the only change (atomic, and before matches what it had cached) it
 * may keep its cached lookups. Otherwise something else may have touched
 * the directory and every cached dentry has to be revalidated.
 *
 * Getting that wrong in one direction serves stale dentries; in the other
 * it throws away the whole dentry cache on every create and unlink.
 * nfs_force_lookup_revalidate() bumps cache_change_attribute by 2, so
 * which way it went is directly observable.
 */

struct reply_fixture {
	struct nfs_inode	nfsi;
	struct nfs_server	server;
	struct nfs_client	client;
	struct super_block	sb;
	struct nfs_rpc_ops	rpc_ops;
	struct nfs_unlinkdata	unlinkdata;
	struct nfs_fattr	dir_attr;
	struct rpc_task		task;
	struct address_space	mapping;
	bool			delegated;
};

static struct reply_fixture *reply_cur;

static int reply_have_delegation(struct inode *inode, fmode_t type, int flags)
{
	return reply_cur->delegated;
}

/*
 * A directory inode with a known change attribute. change_attr_type
 * defaults to MONOTONIC_INCR, which is what a real server almost always
 * advertises; the UNDEFINED case has its own test.
 */
static struct reply_fixture *reply_dir(struct kunit *test, u64 change_attr,
				       umode_t mode)
{
	struct reply_fixture *f;
	struct inode *inode;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->rpc_ops.have_delegation = reply_have_delegation;
	f->client.rpc_ops = &f->rpc_ops;
	f->server.nfs_client = &f->client;
	f->server.change_attr_type = NFS4_CHANGE_TYPE_IS_MONOTONIC_INCR;
	f->sb.s_fs_info = &f->server;

	inode = &f->nfsi.vfs_inode;
	inode->i_sb = &f->sb;
	inode->i_mode = mode;
	spin_lock_init(&inode->i_lock);

	/*
	 * nfs_set_cache_invalid() reads inode->i_mapping->nrpages, so the
	 * inode needs a real address_space. empty_aops is the kernel's own
	 * do-nothing operations table.
	 */
	address_space_init_once(&f->mapping);
	f->mapping.host = inode;
	f->mapping.a_ops = &empty_aops;
	mapping_set_gfp_mask(&f->mapping, GFP_KERNEL);
	inode->i_mapping = &f->mapping;
	inode_set_iversion_raw(inode, change_attr);
	f->nfsi.cache_validity = 0;
	f->nfsi.cache_change_attribute = 100;

	reply_cur = f;
	return f;
}

static void reply_drop_pages(void *mapping)
{
	truncate_inode_pages(mapping, 0);
}

/*
 * Put one real folio in the page cache. Needed wherever a test cares about
 * NFS_INO_INVALID_DATA: nfs_set_cache_invalid() clears that flag when the
 * mapping is empty, on the grounds that there is nothing to invalidate.
 */
static void reply_cache_a_page(struct kunit *test, struct reply_fixture *f)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, 0);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, folio);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, reply_drop_pages,
							&f->mapping), 0);
	KUNIT_ASSERT_EQ(test, filemap_add_folio(&f->mapping, folio, 0,
						GFP_KERNEL), 0);
	folio_unlock(folio);
	folio_put(folio);
}

static u64 dir_change_attr(struct reply_fixture *f)
{
	return inode_peek_iversion_raw(&f->nfsi.vfs_inode);
}

/*
 * The client made the only change: cached lookups are still good, so the
 * dentry cache is left alone.
 */
static void an_atomic_change_keeps_cached_lookups(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };
	unsigned long before = f->nfsi.cache_change_attribute;

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before,
			    "an atomic change threw away the dentry cache");
	KUNIT_EXPECT_EQ(test, dir_change_attr(f), 11ULL);
}

/*
 * The server would not promise atomicity, so another client may have
 * changed the directory: every cached lookup has to be revalidated.
 */
static void a_non_atomic_change_drops_cached_lookups(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 0, .before = 10, .after = 11 };
	unsigned long before = f->nfsi.cache_change_attribute;

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before + 2,
			    "a non-atomic change kept the dentry cache");
}

/*
 * Atomic but the directory was not where the client left it -- someone
 * else changed it in between, so the cached lookups are stale even though
 * the server held it still across this one operation.
 */
static void an_atomic_change_from_an_unexpected_state_drops_lookups(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 7, .after = 11 };
	unsigned long before = f->nfsi.cache_change_attribute;

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before + 2,
			    "trusted an atomic flag over a mismatched change attribute");
}

/* Losing the cached lookups also invalidates the attributes that go with them. */
static void a_non_atomic_change_invalidates_the_attribute_cache(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 0, .before = 10, .after = 11 };

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS);
	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_NLINK);
	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_MODE);
}

/* An atomic change leaves those attributes alone: that is the whole point. */
static void an_atomic_change_spares_the_attribute_cache(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_FALSE_MSG(test,
			       f->nfsi.cache_validity & NFS_INO_INVALID_ACCESS,
			       "an atomic change invalidated the access cache");
	KUNIT_EXPECT_FALSE(test, f->nfsi.cache_validity & NFS_INO_INVALID_NLINK);
}

/*
 * A reply that arrives out of order carries a change attribute the client
 * has already moved past. It must not be rolled backwards.
 */
static void a_stale_reply_does_not_rewind_the_change_attribute(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 20, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 5, .after = 6 };

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, dir_change_attr(f), 20ULL,
			    "a late reply rewound the change attribute");
}

/*
 * When the server will not say how its change attribute behaves, the
 * client cannot assume it increases. Any different value is accepted,
 * including a smaller one; only an identical value is a no-op.
 */
static void an_undefined_change_type_accepts_any_different_value(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 20, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 5, .after = 6 };

	f->server.change_attr_type = NFS4_CHANGE_TYPE_IS_UNDEFINED;
	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, dir_change_attr(f), 6ULL,
			    "an undefined change type refused a lower value");
}

static void an_unchanged_directory_is_left_alone(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 10 };
	unsigned long before = f->nfsi.cache_change_attribute;

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ(test, dir_change_attr(f), 10ULL);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_change_attribute, before);
}

/* Only directories have a dentry cache to drop. */
static void a_regular_file_never_forces_lookup_revalidation(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFREG | 0644);
	struct nfs4_change_info cinfo = { .atomic = 0, .before = 10, .after = 11 };
	unsigned long before = f->nfsi.cache_change_attribute;

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before,
			    "forced lookup revalidation on a regular file");
	KUNIT_EXPECT_FALSE_MSG(test,
			       f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			       "invalidated the data cache of a regular file");
}

/*
 * A directory with cached contents always refetches them, atomic or not:
 * the entries themselves changed even when the cached lookups are still
 * good.
 */
static void a_directory_always_invalidates_its_contents(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	reply_cache_a_page(test, f);
	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_DATA);
}

/*
 * With nothing cached there is nothing to invalidate, so the flag is
 * dropped rather than recorded. Worth pinning: it means the flag cannot be
 * used as a record of "the directory changed", only of "cached data is
 * stale".
 */
static void an_empty_page_cache_drops_the_data_invalidation(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_FALSE_MSG(test,
			       f->nfsi.cache_validity & NFS_INO_INVALID_DATA,
			       "recorded a data invalidation with an empty page cache");
}

/* Holding a delegation over mtime means the timestamps need not be refetched. */
static void a_delegation_spares_the_timestamps(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	f->delegated = true;
	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_FALSE_MSG(test,
			       f->nfsi.cache_validity & NFS_INO_INVALID_MTIME,
			       "refetched mtime while holding a delegation over it");
}

static void without_a_delegation_the_timestamps_are_invalidated(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	nfs4_update_changeattr_locked(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_MTIME);
	KUNIT_EXPECT_TRUE(test, f->nfsi.cache_validity & NFS_INO_INVALID_CTIME);
}

/*
 * The whole completion handler, driven end to end.
 *
 * nfs4_proc_unlink_done() is what the RPC layer calls when a REMOVE reply
 * lands. Given a task carrying a successful status and a reply with no
 * session slot, it runs the sequence check, the error check, and the
 * change-attribute update without any transport at all.
 */

static struct rpc_task *unlink_reply(struct kunit *test,
				     struct reply_fixture *f, int status,
				     u32 atomic, u64 before, u64 after)
{
	struct nfs_removeres *res = &f->unlinkdata.res;

	res->server = &f->server;
	res->dir_attr = &f->dir_attr;
	res->cinfo.atomic = atomic;
	res->cinfo.before = before;
	res->cinfo.after = after;
	/*
	 * No slot: nfs4_sequence_done() takes the NFSv4.0 path and returns
	 * "done" without touching a session.
	 */
	res->seq_res.sr_slot = NULL;

	f->task.tk_status = status;
	f->task.tk_calldata = &f->unlinkdata;
	return &f->task;
}

static void a_successful_unlink_reply_updates_the_directory(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct rpc_task *task = unlink_reply(test, f, 0, 1, 10, 11);

	KUNIT_EXPECT_EQ_MSG(test, nfs4_proc_unlink_done(task, &f->nfsi.vfs_inode),
			    1, "the handler did not report completion");
	KUNIT_EXPECT_EQ(test, dir_change_attr(f), 11ULL);
}

/* The reply's atomic flag reaches the cache decision through the handler. */
static void a_non_atomic_unlink_reply_drops_cached_lookups(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct rpc_task *task = unlink_reply(test, f, 0, 0, 10, 11);
	unsigned long before = f->nfsi.cache_change_attribute;

	KUNIT_ASSERT_EQ(test, nfs4_proc_unlink_done(task, &f->nfsi.vfs_inode), 1);
	KUNIT_EXPECT_EQ(test, f->nfsi.cache_change_attribute, before + 2);
}

static void an_atomic_unlink_reply_keeps_cached_lookups(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct rpc_task *task = unlink_reply(test, f, 0, 1, 10, 11);
	unsigned long before = f->nfsi.cache_change_attribute;

	KUNIT_ASSERT_EQ(test, nfs4_proc_unlink_done(task, &f->nfsi.vfs_inode), 1);
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before,
			    "an atomic unlink reply dropped the dentry cache");
}

/*
 * A failed REMOVE changed nothing on the server, so the client must not
 * advance its change attribute or drop its cached lookups. -EACCES takes
 * the plain-errno path through nfs4_async_handle_exception(): no delay, no
 * recovery, no retry.
 */
static void a_failed_unlink_reply_leaves_the_directory_alone(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct rpc_task *task = unlink_reply(test, f, -EACCES, 1, 10, 11);
	unsigned long before = f->nfsi.cache_change_attribute;

	KUNIT_EXPECT_EQ(test, nfs4_proc_unlink_done(task, &f->nfsi.vfs_inode), 1);
	KUNIT_EXPECT_EQ_MSG(test, dir_change_attr(f), 10ULL,
			    "a failed unlink advanced the change attribute");
	KUNIT_EXPECT_EQ_MSG(test, f->nfsi.cache_change_attribute, before,
			    "a failed unlink dropped the dentry cache");
}


/*
 * Miscellaneous small helpers
 *
 * These are one- or two-line functions with no dependency on RPC state:
 * a bitmask accessor, a "first writer wins" flag setter, and predicates
 * over plain structs. Grouped together because none of them is large
 * enough to deserve its own suite.
 */

/* The first GETATTR reply sets the pre-op change attribute; later ones don't. */
static void fattr_set_prechange_only_sets_it_once(struct kunit *test)
{
	struct nfs_fattr fattr = {};

	nfs4_fattr_set_prechange(&fattr, 5);
	KUNIT_EXPECT_EQ(test, fattr.pre_change_attr, 5ULL);
	KUNIT_EXPECT_TRUE(test, fattr.valid & NFS_ATTR_FATTR_PRECHANGE);

	nfs4_fattr_set_prechange(&fattr, 9);
	KUNIT_EXPECT_EQ_MSG(test, fattr.pre_change_attr, 5ULL,
			    "a second call overwrote the pre-change value");
}

/*
 * exception_should_retrans(): NFS_MOUNT_SOFTERR retry ceiling
 *
 * nfs_delay_retrans defaults to -1 (disabled). Save and restore it since
 * it is a module-wide variable, not per-call state.
 */

static void retrans_disabled_by_default_never_gives_up(struct kunit *test)
{
	struct nfs_server server = { .flags = NFS_MOUNT_SOFTERR };
	struct nfs4_exception exception = {};
	short saved = nfs_delay_retrans;

	nfs_delay_retrans = -1;
	KUNIT_EXPECT_EQ(test, nfs4_exception_should_retrans(&server, &exception), 0);
	nfs_delay_retrans = saved;
}

static void softerr_off_ignores_the_retrans_ceiling(struct kunit *test)
{
	struct nfs_server server = {};
	struct nfs4_exception exception = {};
	short saved = nfs_delay_retrans;

	nfs_delay_retrans = 0;
	KUNIT_EXPECT_EQ_MSG(test,
			    nfs4_exception_should_retrans(&server, &exception),
			    0, "retrans ceiling applied without NFS_MOUNT_SOFTERR");
	nfs_delay_retrans = saved;
}

static void softerr_gives_up_once_the_ceiling_is_reached(struct kunit *test)
{
	struct nfs_server server = { .flags = NFS_MOUNT_SOFTERR };
	struct nfs4_exception exception = {};
	short saved = nfs_delay_retrans;
	int i;

	/*
	 * retrans is post-incremented and compared >= ceiling, so a ceiling
	 * of 2 permits two attempts (retrans 0 and 1) and gives up on the
	 * third (retrans 2).
	 */
	nfs_delay_retrans = 2;
	for (i = 0; i < 2; i++)
		KUNIT_ASSERT_EQ_MSG(test,
				    nfs4_exception_should_retrans(&server, &exception),
				    0, "gave up before reaching the ceiling");
	KUNIT_EXPECT_EQ_MSG(test,
			    nfs4_exception_should_retrans(&server, &exception),
			    -EAGAIN, "did not give up once past the ceiling");
	nfs_delay_retrans = saved;
}

/* Only krb5i and krb5p provide integrity; krb5 alone (auth only) does not. */
static void integrity_protection_requires_krb5i_or_krb5p(struct kunit *test)
{
	struct rpc_auth auth_i = { .au_flavor = RPC_AUTH_GSS_KRB5I };
	struct rpc_auth auth_p = { .au_flavor = RPC_AUTH_GSS_KRB5P };
	struct rpc_auth auth_plain = { .au_flavor = RPC_AUTH_GSS_KRB5 };
	struct rpc_clnt clnt_i = { .cl_auth = &auth_i };
	struct rpc_clnt clnt_p = { .cl_auth = &auth_p };
	struct rpc_clnt clnt_plain = { .cl_auth = &auth_plain };
	struct nfs_client client = {};

	client.cl_rpcclient = &clnt_i;
	KUNIT_EXPECT_TRUE(test, _nfs4_is_integrity_protected(&client));
	client.cl_rpcclient = &clnt_p;
	KUNIT_EXPECT_TRUE(test, _nfs4_is_integrity_protected(&client));
	client.cl_rpcclient = &clnt_plain;
	KUNIT_EXPECT_FALSE_MSG(test, _nfs4_is_integrity_protected(&client),
			       "plain krb5 auth was reported as integrity-protected");
}

/*
 * A sequence result with no slot needs neither a session nor a slot table
 * to be processed: NFSv4.0 requests never attach one.
 */
static void sequence_process_with_no_slot_reports_done(struct kunit *test)
{
	struct nfs4_sequence_res res = { .sr_slot = NULL };

	KUNIT_EXPECT_EQ(test, nfs4_sequence_process(NULL, &res), 1);
}

/* Freeing a result with no slot is a no-op, not a NULL dereference. */
static void sequence_free_slot_with_no_slot_is_a_no_op(struct kunit *test)
{
	struct nfs4_sequence_res res = { .sr_slot = NULL };

	nfs4_sequence_free_slot(&res);
}

/*
 * A server whose v1 atomic-open capability turns out not to work signals
 * -EINVAL; the client drops the capability and retries without it. Any
 * other error, or a server that never claimed the capability, is left
 * alone.
 */
static void einval_on_a_v1_capable_server_disables_the_capability(struct kunit *test)
{
	struct nfs_server server = { .caps = NFS_CAP_ATOMIC_OPEN_V1 };
	struct nfs4_exception exception = {};

	KUNIT_EXPECT_TRUE(test,
			  nfs4_clear_cap_atomic_open_v1(&server, -EINVAL, &exception));
	KUNIT_EXPECT_FALSE_MSG(test, server.caps & NFS_CAP_ATOMIC_OPEN_V1,
			       "the capability was not cleared");
	KUNIT_EXPECT_TRUE(test, exception.retry);
}

static void einval_without_the_capability_is_left_alone(struct kunit *test)
{
	struct nfs_server server = { .caps = 0 };
	struct nfs4_exception exception = {};

	KUNIT_EXPECT_FALSE(test,
			   nfs4_clear_cap_atomic_open_v1(&server, -EINVAL, &exception));
	KUNIT_EXPECT_FALSE(test, exception.retry);
}

static void a_different_error_never_clears_the_capability(struct kunit *test)
{
	struct nfs_server server = { .caps = NFS_CAP_ATOMIC_OPEN_V1 };
	struct nfs4_exception exception = {};

	KUNIT_EXPECT_FALSE(test,
			   nfs4_clear_cap_atomic_open_v1(&server, -EACCES, &exception));
	KUNIT_EXPECT_TRUE_MSG(test, server.caps & NFS_CAP_ATOMIC_OPEN_V1,
			      "an unrelated error cleared the capability");
}

static struct kunit_case nfs4_misc_cases[] = {
	KUNIT_CASE(fattr_set_prechange_only_sets_it_once),
	KUNIT_CASE(retrans_disabled_by_default_never_gives_up),
	KUNIT_CASE(softerr_off_ignores_the_retrans_ceiling),
	KUNIT_CASE(softerr_gives_up_once_the_ceiling_is_reached),
	KUNIT_CASE(integrity_protection_requires_krb5i_or_krb5p),
	KUNIT_CASE(sequence_process_with_no_slot_reports_done),
	KUNIT_CASE(sequence_free_slot_with_no_slot_is_a_no_op),
	KUNIT_CASE(einval_on_a_v1_capable_server_disables_the_capability),
	KUNIT_CASE(einval_without_the_capability_is_left_alone),
	KUNIT_CASE(a_different_error_never_clears_the_capability),
	{}
};

static struct kunit_suite nfs4_misc_suite = {
	.name		= "nfs4-misc",
	.test_cases	= nfs4_misc_cases,
};

/*
 * Open-state accounting: struct nfs4_state's n_rdonly/n_wronly/n_rdwr
 * reference counts and the NFS_O_*_STATE flags that mirror them.
 *
 * Every open of the same file by the same owner shares one nfs4_state.
 * The n_* counters track how many callers hold each mode so that closing
 * one does not close the state for the others; the flags track which
 * modes the *stateid on the wire* currently covers, which can lag the
 * counters when an OPEN is in flight. can_open_cached() is what decides
 * whether a second open can be satisfied without a trip to the server.
 */

static struct nfs4_state_owner *state_owner(struct kunit *test)
{
	struct nfs4_state_owner *owner;

	owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, owner);
	INIT_LIST_HEAD(&owner->so_states);
	spin_lock_init(&owner->so_lock);
	return owner;
}

static struct nfs4_state *open_state(struct kunit *test)
{
	struct nfs4_state *state;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
	INIT_LIST_HEAD(&state->open_states);
	seqlock_init(&state->seqlock);
	state->owner = state_owner(test);
	return state;
}

static void mode_match_reads_the_right_counter(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	state->n_rdonly = 1;
	KUNIT_EXPECT_TRUE(test, nfs4_mode_match_open_stateid(state, FMODE_READ));
	KUNIT_EXPECT_FALSE(test, nfs4_mode_match_open_stateid(state, FMODE_WRITE));

	state->n_wronly = 1;
	KUNIT_EXPECT_TRUE(test, nfs4_mode_match_open_stateid(state, FMODE_WRITE));

	state->n_rdwr = 1;
	KUNIT_EXPECT_TRUE(test,
			  nfs4_mode_match_open_stateid(state, FMODE_READ | FMODE_WRITE));
}

/* O_EXCL and O_TRUNC always need a fresh trip to the server. */
static void o_excl_and_o_trunc_never_use_the_cache(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	state->n_rdonly = 1;

	KUNIT_EXPECT_FALSE(test,
			   can_open_cached(state, FMODE_READ, O_EXCL, NFS4_OPEN_CLAIM_PREVIOUS));
	KUNIT_EXPECT_FALSE(test,
			   can_open_cached(state, FMODE_READ, O_TRUNC, NFS4_OPEN_CLAIM_PREVIOUS));
}

/*
 * NULL and FH claims are a fresh lookup, so caching never applies to them
 * regardless of what the state already holds.
 */
static void null_and_fh_claims_never_use_the_cache(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	state->n_rdonly = 1;

	KUNIT_EXPECT_FALSE(test,
			   can_open_cached(state, FMODE_READ, 0, NFS4_OPEN_CLAIM_NULL));
	KUNIT_EXPECT_FALSE(test,
			   can_open_cached(state, FMODE_READ, 0, NFS4_OPEN_CLAIM_FH));
}

/*
 * Otherwise the cache may be used, but only if the wire stateid already
 * covers the requested mode -- the flag, not just the counter.
 */
static void other_claims_use_the_cache_when_the_flag_is_set(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	state->n_rdonly = 1;
	KUNIT_EXPECT_FALSE_MSG(test,
			       can_open_cached(state, FMODE_READ, 0,
						NFS4_OPEN_CLAIM_PREVIOUS),
			       "used the cache before the stateid flag was set");

	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	KUNIT_EXPECT_TRUE(test,
			  can_open_cached(state, FMODE_READ, 0,
					   NFS4_OPEN_CLAIM_PREVIOUS));
}

/* A delegation of the wrong file mode cannot satisfy this open. */
static void delegation_of_the_wrong_mode_cannot_be_used(struct kunit *test)
{
	struct nfs_delegation delegation = { .type = FMODE_READ };

	KUNIT_EXPECT_FALSE(test,
			   can_open_delegated(&delegation, FMODE_WRITE,
					      NFS4_OPEN_CLAIM_NULL));
}

static void an_absent_delegation_cannot_be_used(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   can_open_delegated(NULL, FMODE_READ, NFS4_OPEN_CLAIM_NULL));
}

/*
 * CLAIM_PREVIOUS during reboot recovery may only use a delegation that
 * does not itself need reclaiming -- otherwise the client would be
 * vouching for state the server has already forgotten.
 */
static void claim_previous_needs_a_delegation_that_does_not_need_reclaim(struct kunit *test)
{
	struct nfs_delegation delegation = { .type = FMODE_READ };

	KUNIT_EXPECT_TRUE(test,
			  can_open_delegated(&delegation, FMODE_READ,
					     NFS4_OPEN_CLAIM_PREVIOUS));

	set_bit(NFS_DELEGATION_NEED_RECLAIM, &delegation.flags);
	KUNIT_EXPECT_FALSE_MSG(test,
			       can_open_delegated(&delegation, FMODE_READ,
						  NFS4_OPEN_CLAIM_PREVIOUS),
			       "used a delegation that itself needs reclaiming");
}

/* Delegation-based claims (recovery paths) are refused outright. */
static void delegation_claims_are_never_satisfied_by_a_delegation(struct kunit *test)
{
	struct nfs_delegation delegation = { .type = FMODE_READ | FMODE_WRITE };

	KUNIT_EXPECT_FALSE(test,
			   can_open_delegated(&delegation, FMODE_READ,
					      NFS4_OPEN_CLAIM_DELEGATE_CUR));
}

/* update_open_stateflags() bumps the right counter and merges the mode in. */
static void update_stateflags_increments_the_matching_counter(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	update_open_stateflags(state, FMODE_READ);
	KUNIT_EXPECT_EQ(test, state->n_rdonly, 1U);
	KUNIT_EXPECT_EQ(test, state->n_wronly, 0U);

	update_open_stateflags(state, FMODE_WRITE);
	KUNIT_EXPECT_EQ(test, state->n_wronly, 1U);

	update_open_stateflags(state, FMODE_READ);
	KUNIT_EXPECT_EQ_MSG(test, state->n_rdonly, 2U,
			    "a second read open did not add to the counter");
}

static void update_stateflags_merges_the_mode_bits(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	update_open_stateflags(state, FMODE_READ);
	update_open_stateflags(state, FMODE_WRITE);

	KUNIT_EXPECT_EQ(test, state->state, (fmode_t)(FMODE_READ | FMODE_WRITE));
}

/*
 * The counters can run ahead of the flags while an OPEN upgrade is in
 * flight -- that gap is exactly what recovery has to close.
 */
static void recover_openmode_detects_a_counter_ahead_of_its_flag(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	KUNIT_EXPECT_FALSE(test, nfs_open_stateid_recover_openmode(state));

	state->n_wronly = 1;
	KUNIT_EXPECT_TRUE_MSG(test, nfs_open_stateid_recover_openmode(state),
			      "a counter ahead of its flag was not detected");

	set_bit(NFS_O_WRONLY_STATE, &state->flags);
	KUNIT_EXPECT_FALSE_MSG(test, nfs_open_stateid_recover_openmode(state),
			       "recovery was still signalled once the flag caught up");
}

/* Logging an update wakes anyone waiting on the state's stateid, once. */
static void log_update_wakes_only_when_the_wait_flag_was_set(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	init_waitqueue_head(&state->waitq);

	/* No flag set: nothing to wake, and no crash from an empty queue. */
	nfs_state_log_update_open_stateid(state);

	set_bit(NFS_STATE_CHANGE_WAIT, &state->flags);
	nfs_state_log_update_open_stateid(state);
	KUNIT_EXPECT_FALSE_MSG(test, test_bit(NFS_STATE_CHANGE_WAIT, &state->flags),
			       "the wait flag was not cleared after waking waiters");
}

/* With no opens outstanding there is nothing to resync. */
static void resync_with_no_opens_does_not_set_the_open_flag(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	nfs_resync_open_stateid_locked(state);
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_OPEN_STATE, &state->flags));
}

/* Each outstanding counter gets its matching flag set back. */
static void resync_sets_a_flag_for_every_outstanding_counter(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	state->n_rdonly = 1;
	state->n_rdwr = 1;

	nfs_resync_open_stateid_locked(state);

	KUNIT_EXPECT_TRUE(test, test_bit(NFS_O_RDONLY_STATE, &state->flags));
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_O_RDWR_STATE, &state->flags));
	KUNIT_EXPECT_FALSE_MSG(test, test_bit(NFS_O_WRONLY_STATE, &state->flags),
			       "set a flag for a counter that was never incremented");
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_OPEN_STATE, &state->flags));
}

/* Dropping to no mode at all clears every open-related flag. */
static void clear_open_stateid_to_no_mode_clears_every_flag(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	set_bit(NFS_O_WRONLY_STATE, &state->flags);
	set_bit(NFS_O_RDWR_STATE, &state->flags);
	set_bit(NFS_OPEN_STATE, &state->flags);

	nfs_clear_open_stateid_locked(state, NULL, 0);

	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_RDONLY_STATE, &state->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_WRONLY_STATE, &state->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_RDWR_STATE, &state->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_OPEN_STATE, &state->flags));
}

/*
 * Downgrading to write-only leaves the read-write flag cleared (no caller
 * has that combined mode any more) but leaves NFS_OPEN_STATE itself set,
 * since some mode is still held.
 */
static void clear_open_stateid_downgrade_keeps_the_open_flag(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	set_bit(NFS_O_RDWR_STATE, &state->flags);
	set_bit(NFS_OPEN_STATE, &state->flags);

	nfs_clear_open_stateid_locked(state, NULL, FMODE_WRITE);

	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_RDWR_STATE, &state->flags));
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(NFS_OPEN_STATE, &state->flags),
			      "downgrading to a single mode cleared the open flag");
}

/* Clearing every open flag is independent of what state a lock might hold. */
static void state_clear_open_flags_leaves_nothing_set(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);

	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	set_bit(NFS_O_WRONLY_STATE, &state->flags);
	set_bit(NFS_O_RDWR_STATE, &state->flags);
	set_bit(NFS_OPEN_STATE, &state->flags);
	set_bit(NFS_DELEGATED_STATE, &state->flags);

	nfs_state_clear_open_state_flags(state);

	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_RDONLY_STATE, &state->flags));
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_OPEN_STATE, &state->flags));
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(NFS_DELEGATED_STATE, &state->flags),
			      "cleared a flag it is not responsible for");
}

/* Setting a delegation copies its stateid in and raises the flag. */
static void set_delegation_copies_the_stateid_and_raises_the_flag(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid deleg = { .seqid = cpu_to_be32(7) };

	nfs_state_set_delegation(state, &deleg, FMODE_READ);

	KUNIT_EXPECT_TRUE(test, nfs4_stateid_match(&state->stateid, &deleg));
	KUNIT_EXPECT_TRUE(test, test_bit(NFS_DELEGATED_STATE, &state->flags));
}

/* Clearing it reverts the current stateid to the OPEN stateid. */
static void clear_delegation_reverts_to_the_open_stateid(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid deleg = { .seqid = cpu_to_be32(7) };

	state->open_stateid.seqid = cpu_to_be32(3);
	nfs_state_set_delegation(state, &deleg, FMODE_READ);
	nfs_state_clear_delegation(state);

	KUNIT_EXPECT_TRUE_MSG(test,
			      nfs4_stateid_match(&state->stateid, &state->open_stateid),
			      "the current stateid was not reverted to the open stateid");
	KUNIT_EXPECT_FALSE(test, test_bit(NFS_DELEGATED_STATE, &state->flags));
}

static struct kunit_case nfs4_open_state_cases[] = {
	KUNIT_CASE(mode_match_reads_the_right_counter),
	KUNIT_CASE(o_excl_and_o_trunc_never_use_the_cache),
	KUNIT_CASE(null_and_fh_claims_never_use_the_cache),
	KUNIT_CASE(other_claims_use_the_cache_when_the_flag_is_set),
	KUNIT_CASE(delegation_of_the_wrong_mode_cannot_be_used),
	KUNIT_CASE(an_absent_delegation_cannot_be_used),
	KUNIT_CASE(claim_previous_needs_a_delegation_that_does_not_need_reclaim),
	KUNIT_CASE(delegation_claims_are_never_satisfied_by_a_delegation),
	KUNIT_CASE(update_stateflags_increments_the_matching_counter),
	KUNIT_CASE(update_stateflags_merges_the_mode_bits),
	KUNIT_CASE(recover_openmode_detects_a_counter_ahead_of_its_flag),
	KUNIT_CASE(log_update_wakes_only_when_the_wait_flag_was_set),
	KUNIT_CASE(resync_with_no_opens_does_not_set_the_open_flag),
	KUNIT_CASE(resync_sets_a_flag_for_every_outstanding_counter),
	KUNIT_CASE(clear_open_stateid_to_no_mode_clears_every_flag),
	KUNIT_CASE(clear_open_stateid_downgrade_keeps_the_open_flag),
	KUNIT_CASE(state_clear_open_flags_leaves_nothing_set),
	KUNIT_CASE(set_delegation_copies_the_stateid_and_raises_the_flag),
	KUNIT_CASE(clear_delegation_reverts_to_the_open_stateid),
	{}
};

static struct kunit_suite nfs4_open_state_suite = {
	.name		= "nfs4-open-state",
	.test_cases	= nfs4_open_state_cases,
};

/*
 * EXCHANGE_ID flag validation, server-scope comparison, and session
 * channel attribute negotiation. All pure, all decode-adjacent: these run
 * on values the XDR layer has already parsed out of an EXCHANGE_ID or
 * CREATE_SESSION reply.
 */

static void a_bare_pnfs_mds_flag_is_accepted(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			nfs4_check_cl_exchange_flags(EXCHGID4_FLAG_USE_PNFS_MDS, 1),
			NFS_OK);
}

/* Claiming to be both a pNFS metadata server and a non-pNFS one is invalid. */
static void mds_and_non_pnfs_together_is_invalid(struct kunit *test)
{
	u32 flags = EXCHGID4_FLAG_USE_PNFS_MDS | EXCHGID4_FLAG_USE_NON_PNFS;

	KUNIT_EXPECT_EQ(test, nfs4_check_cl_exchange_flags(flags, 1),
			-NFS4ERR_INVAL);
}

/* At least one of the pNFS role flags must be present. */
static void no_pnfs_role_flag_at_all_is_invalid(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_check_cl_exchange_flags(0, 1), -NFS4ERR_INVAL);
}

/*
 * Minor version 2 accepts a wider flag mask than 1.x; a flag valid under
 * 2 but not under 1.x is rejected when negotiating the older minor
 * version.
 */
static void a_flag_valid_under_v2_is_rejected_under_v1(struct kunit *test)
{
	u32 v2_only_bit = ~EXCHGID4_FLAG_MASK_R & EXCHGID4_2_FLAG_MASK_R &
			  EXCHGID4_FLAG_USE_PNFS_MDS;
	u32 flags = EXCHGID4_FLAG_USE_PNFS_MDS | v2_only_bit;

	/* Skip if this kernel's mask difference does not isolate a bit. */
	if (!v2_only_bit)
		kunit_skip(test, "no minor-version-only bit in this mask");

	KUNIT_EXPECT_EQ(test, nfs4_check_cl_exchange_flags(flags, 2), NFS_OK);
	KUNIT_EXPECT_EQ(test, nfs4_check_cl_exchange_flags(flags, 1),
			-NFS4ERR_INVAL);
}

static void identical_server_scopes_match(struct kunit *test)
{
	struct nfs41_server_scope a = { .server_scope_sz = 3, .server_scope = "abc" };
	struct nfs41_server_scope b = { .server_scope_sz = 3, .server_scope = "abc" };

	KUNIT_EXPECT_TRUE(test, nfs41_same_server_scope(&a, &b));
}

static void different_lengths_never_match(struct kunit *test)
{
	struct nfs41_server_scope a = { .server_scope_sz = 3, .server_scope = "abc" };
	struct nfs41_server_scope b = { .server_scope_sz = 4, .server_scope = "abcd" };

	KUNIT_EXPECT_FALSE_MSG(test, nfs41_same_server_scope(&a, &b),
			       "compared bytes past a shorter scope's length");
}

static void same_length_different_bytes_do_not_match(struct kunit *test)
{
	struct nfs41_server_scope a = { .server_scope_sz = 3, .server_scope = "abc" };
	struct nfs41_server_scope b = { .server_scope_sz = 3, .server_scope = "abd" };

	KUNIT_EXPECT_FALSE(test, nfs41_same_server_scope(&a, &b));
}

/*
 * Fore-channel verification: the server may not promise more than the
 * client offered, in either direction of the "how much can I send you"
 * negotiation.
 */
static void fore_channel_response_exceeding_max_resp_sz_is_rejected(struct kunit *test)
{
	struct nfs41_create_session_args args = { .fc_attrs = { .max_resp_sz = 100 } };
	struct nfs41_create_session_res res = { .fc_attrs = { .max_resp_sz = 200,
							      .max_ops = 2,
							      .max_reqs = 1 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_fore_channel_attrs(&args, &res), -EINVAL);
}

/*
 * The client's requested max_ops is the minimum it can work with, since
 * it may not be able to break a compound into smaller pieces than that.
 */
static void fore_channel_max_ops_below_the_request_is_rejected(struct kunit *test)
{
	struct nfs41_create_session_args args = { .fc_attrs = { .max_ops = 8 } };
	struct nfs41_create_session_res res = { .fc_attrs = { .max_ops = 4,
							      .max_reqs = 1 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_fore_channel_attrs(&args, &res), -EINVAL);
}

static void fore_channel_zero_max_reqs_is_rejected(struct kunit *test)
{
	struct nfs41_create_session_args args = {};
	struct nfs41_create_session_res res = {};

	KUNIT_EXPECT_EQ(test, nfs4_verify_fore_channel_attrs(&args, &res), -EINVAL);
}

/* An oversized max_reqs is silently clamped rather than rejected. */
static void fore_channel_oversized_max_reqs_is_clamped_not_rejected(struct kunit *test)
{
	struct nfs41_create_session_args args = { .fc_attrs = { .max_ops = 2 } };
	struct nfs41_create_session_res res = { .fc_attrs = {
			.max_ops = 2, .max_reqs = NFS4_MAX_SLOT_TABLE + 100 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_fore_channel_attrs(&args, &res), 0);
	KUNIT_EXPECT_EQ_MSG(test, res.fc_attrs.max_reqs, (u32)NFS4_MAX_SLOT_TABLE,
			    "an oversized max_reqs was not clamped");
}

/* Without a back channel, none of the back-channel fields are checked. */
static void no_back_channel_skips_verification_entirely(struct kunit *test)
{
	struct nfs41_create_session_args args = {};
	struct nfs41_create_session_res res = { .flags = 0,
						.bc_attrs = { .max_reqs = 99999 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_back_channel_attrs(&args, &res), 0);
}

/* With a back channel, the server may not exceed what was offered. */
static void back_channel_exceeding_max_reqs_is_rejected(struct kunit *test)
{
	struct nfs41_create_session_args args = { .bc_attrs = { .max_reqs = 1 } };
	struct nfs41_create_session_res res = { .flags = SESSION4_BACK_CHAN,
						.bc_attrs = { .max_reqs = 2 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_back_channel_attrs(&args, &res), -EINVAL);
}

/* The combined check runs fore before back, so a fore failure is reported first. */
static void combined_verification_checks_fore_before_back(struct kunit *test)
{
	struct nfs41_create_session_args args = {};
	struct nfs41_create_session_res res = { .fc_attrs = { .max_reqs = 0 },
						.bc_attrs = { .max_reqs = 0 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_channel_attrs(&args, &res), -EINVAL);
}

/*
 * A fore-channel pass must not short-circuit the back-channel check: the
 * combined function has to actually call both, not just propagate a
 * successful fore-channel result.
 */
static void combined_verification_still_checks_back_after_fore_passes(struct kunit *test)
{
	struct nfs41_create_session_args args = { .fc_attrs = { .max_ops = 1 },
						  .bc_attrs = { .max_reqs = 1 } };
	struct nfs41_create_session_res res = { .flags = SESSION4_BACK_CHAN,
						.fc_attrs = { .max_ops = 1,
							     .max_reqs = 1 },
						.bc_attrs = { .max_reqs = 2 } };

	KUNIT_EXPECT_EQ_MSG(test, nfs4_verify_channel_attrs(&args, &res), -EINVAL,
			    "a fore-channel pass suppressed the back-channel check");
}

static void combined_verification_passes_when_both_channels_are_fine(struct kunit *test)
{
	struct nfs41_create_session_args args = { .fc_attrs = { .max_ops = 1 } };
	struct nfs41_create_session_res res = { .fc_attrs = { .max_ops = 1,
							      .max_reqs = 1 } };

	KUNIT_EXPECT_EQ(test, nfs4_verify_channel_attrs(&args, &res), 0);
}

static struct kunit_case nfs4_session_negotiation_cases[] = {
	KUNIT_CASE(a_bare_pnfs_mds_flag_is_accepted),
	KUNIT_CASE(mds_and_non_pnfs_together_is_invalid),
	KUNIT_CASE(no_pnfs_role_flag_at_all_is_invalid),
	KUNIT_CASE(a_flag_valid_under_v2_is_rejected_under_v1),
	KUNIT_CASE(identical_server_scopes_match),
	KUNIT_CASE(different_lengths_never_match),
	KUNIT_CASE(same_length_different_bytes_do_not_match),
	KUNIT_CASE(fore_channel_response_exceeding_max_resp_sz_is_rejected),
	KUNIT_CASE(fore_channel_max_ops_below_the_request_is_rejected),
	KUNIT_CASE(fore_channel_zero_max_reqs_is_rejected),
	KUNIT_CASE(fore_channel_oversized_max_reqs_is_clamped_not_rejected),
	KUNIT_CASE(no_back_channel_skips_verification_entirely),
	KUNIT_CASE(back_channel_exceeding_max_reqs_is_rejected),
	KUNIT_CASE(combined_verification_checks_fore_before_back),
	KUNIT_CASE(combined_verification_still_checks_back_after_fore_passes),
	KUNIT_CASE(combined_verification_passes_when_both_channels_are_fine),
	{}
};

static struct kunit_suite nfs4_session_negotiation_suite = {
	.name		= "nfs4-session-negotiation",
	.test_cases	= nfs4_session_negotiation_cases,
};

/*
 * Stateid comparison and status classification
 */

/*
 * nfs41_match_stateid() treats a zero seqid as a wildcard: it appears in
 * "any state" stateids used by RELEASE_LOCKOWNER and friends, so a zero
 * on either side matches any seqid on the other.
 */
static void nfs41_match_treats_a_zero_seqid_as_a_wildcard(struct kunit *test)
{
	nfs4_stateid a = { .type = NFS4_LOCK_STATEID_TYPE, .seqid = 0 };
	nfs4_stateid b = { .type = NFS4_LOCK_STATEID_TYPE, .seqid = cpu_to_be32(9) };

	memset(a.other, 0x42, sizeof(a.other));
	memset(b.other, 0x42, sizeof(b.other));

	KUNIT_EXPECT_TRUE(test, nfs41_match_stateid(&a, &b));
}

static void nfs41_match_requires_the_same_other_field(struct kunit *test)
{
	nfs4_stateid a = { .type = NFS4_LOCK_STATEID_TYPE };
	nfs4_stateid b = { .type = NFS4_LOCK_STATEID_TYPE };

	memset(a.other, 0x11, sizeof(a.other));
	memset(b.other, 0x22, sizeof(b.other));

	KUNIT_EXPECT_FALSE(test, nfs41_match_stateid(&a, &b));
}

static void nfs41_match_requires_the_same_type(struct kunit *test)
{
	nfs4_stateid a = { .type = NFS4_LOCK_STATEID_TYPE };
	nfs4_stateid b = { .type = NFS4_OPEN_STATEID_TYPE };

	KUNIT_EXPECT_FALSE_MSG(test, nfs41_match_stateid(&a, &b),
			       "stateids of different types were treated as equal");
}

/*
 * nfs4_match_stateid() delegates to nfs4_stateid_match(), which -- unlike
 * nfs41_match_stateid() above -- requires an exact match: same type, and
 * the full 16-byte payload including seqid, with no zero-seqid wildcard.
 * Recorded as the contrast: two functions with almost the same name and a
 * real difference in what they consider equal.
 */
static void nfs4_match_requires_an_exact_seqid(struct kunit *test)
{
	nfs4_stateid a = { .type = NFS4_LOCK_STATEID_TYPE, .seqid = cpu_to_be32(1) };
	nfs4_stateid b = { .type = NFS4_LOCK_STATEID_TYPE, .seqid = cpu_to_be32(9) };

	memset(a.other, 0x77, sizeof(a.other));
	memset(b.other, 0x77, sizeof(b.other));

	KUNIT_EXPECT_FALSE_MSG(test, nfs4_match_stateid(&a, &b),
			       "differing seqids matched with no wildcard rule");

	b.seqid = a.seqid;
	KUNIT_EXPECT_TRUE(test, nfs4_match_stateid(&a, &b));
}

static void nfs4_match_requires_the_same_type_too(struct kunit *test)
{
	nfs4_stateid a = { .type = NFS4_LOCK_STATEID_TYPE, .seqid = cpu_to_be32(1) };
	nfs4_stateid b = { .type = NFS4_OPEN_STATEID_TYPE, .seqid = cpu_to_be32(1) };

	memset(a.other, 0x77, sizeof(a.other));
	memset(b.other, 0x77, sizeof(b.other));

	KUNIT_EXPECT_FALSE(test, nfs4_match_stateid(&a, &b));
}

/*
 * Every status nfs4_error_stateid_expired() recognises means the same
 * thing operationally: the stateid the client is holding is no longer
 * valid on the server and open/lock recovery has to run.
 */
struct stateid_expired_param {
	const char	*desc;
	int		err;
	bool		expected;
};

static void stateid_expired_get_desc(const struct stateid_expired_param *p, char *desc)
{
	strscpy(desc, p->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct stateid_expired_param stateid_expired_params[] = {
	{ "DELEG_REVOKED",	-NFS4ERR_DELEG_REVOKED,	true },
	{ "ADMIN_REVOKED",	-NFS4ERR_ADMIN_REVOKED,	true },
	{ "BAD_STATEID",	-NFS4ERR_BAD_STATEID,		true },
	{ "STALE_STATEID",	-NFS4ERR_STALE_STATEID,	true },
	{ "OLD_STATEID",	-NFS4ERR_OLD_STATEID,		true },
	{ "OPENMODE",		-NFS4ERR_OPENMODE,		true },
	{ "EXPIRED",		-NFS4ERR_EXPIRED,		true },
	{ "unrelated error",	-EACCES,			false },
	{ "success",		0,				false },
	{ "GRACE is not expiry", -NFS4ERR_GRACE,		false },
};

KUNIT_ARRAY_PARAM(stateid_expired, stateid_expired_params, stateid_expired_get_desc);

static void stateid_expired_case(struct kunit *test)
{
	const struct stateid_expired_param *p = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, nfs4_error_stateid_expired(p->err), p->expected,
			    "classifying %d", p->err);
}

/*
 * fs4_path comparison, used to detect a referral or migration target
 * that is actually the same location the client is already using.
 */

/*
 * NFS4_PATHNAME_MAXCOMPONENTS is 512, so struct nfs4_pathname is too large
 * for a kernel thread's stack -- allocated rather than declared inline.
 */
static struct nfs4_pathname *alloc_pathname(struct kunit *test)
{
	struct nfs4_pathname *path;

	path = kunit_kzalloc(test, sizeof(*path), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	return path;
}

static void identical_pathnames_are_the_same(struct kunit *test)
{
	struct nfs4_pathname *path = alloc_pathname(test);

	path->ncomponents = 2;
	path->components[0].data = "export";
	path->components[0].len = 6;
	path->components[1].data = "home";
	path->components[1].len = 4;

	KUNIT_EXPECT_TRUE(test, _is_same_nfs4_pathname(path, path));
}

static void different_component_counts_are_different_paths(struct kunit *test)
{
	struct nfs4_pathname *a = alloc_pathname(test);
	struct nfs4_pathname *b = alloc_pathname(test);

	a->ncomponents = 1;
	a->components[0].data = "export";
	a->components[0].len = 6;

	b->ncomponents = 2;
	b->components[0].data = "export";
	b->components[0].len = 6;
	b->components[1].data = "home";
	b->components[1].len = 4;

	KUNIT_EXPECT_FALSE(test, _is_same_nfs4_pathname(a, b));
}

/* Same component count and lengths, but different bytes. */
static void differing_component_bytes_are_different_paths(struct kunit *test)
{
	struct nfs4_pathname *a = alloc_pathname(test);
	struct nfs4_pathname *b = alloc_pathname(test);

	a->ncomponents = 1;
	a->components[0].data = "export";
	a->components[0].len = 6;

	b->ncomponents = 1;
	b->components[0].data = "exp0rt";
	b->components[0].len = 6;

	KUNIT_EXPECT_FALSE(test, _is_same_nfs4_pathname(a, b));
}

static struct kunit_case nfs4_stateid_status_cases[] = {
	KUNIT_CASE(nfs41_match_treats_a_zero_seqid_as_a_wildcard),
	KUNIT_CASE(nfs41_match_requires_the_same_other_field),
	KUNIT_CASE(nfs41_match_requires_the_same_type),
	KUNIT_CASE(nfs4_match_requires_an_exact_seqid),
	KUNIT_CASE(nfs4_match_requires_the_same_type_too),
	{
		.name			= "stateid expired classification",
		.run_case		= stateid_expired_case,
		.generate_params	= stateid_expired_gen_params,
	},
	KUNIT_CASE(identical_pathnames_are_the_same),
	KUNIT_CASE(different_component_counts_are_different_paths),
	KUNIT_CASE(differing_component_bytes_are_different_paths),
	{}
};

static struct kunit_suite nfs4_stateid_status_suite = {
	.name		= "nfs4-stateid-status",
	.test_cases	= nfs4_stateid_status_cases,
};


/*
 * Sequence bookkeeping and lease renewal
 */

static void init_sequence_starts_with_no_slot(struct kunit *test)
{
	struct nfs4_sequence_args args = { .sa_slot = (void *)1, .sa_cache_this = 9 };
	struct nfs4_sequence_res res = { .sr_slot = (void *)1 };

	nfs4_init_sequence(&args, &res, 1, 1);

	KUNIT_EXPECT_PTR_EQ(test, args.sa_slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, res.sr_slot, NULL);
	/* sa_cache_this/sa_privileged are 1-bit fields: no typeof() macros. */
	KUNIT_EXPECT_TRUE(test, args.sa_cache_this);
	KUNIT_EXPECT_TRUE(test, args.sa_privileged);
}

/* A NULL slot is a no-op: nothing to attach. */
static void attach_slot_with_no_slot_is_a_no_op(struct kunit *test)
{
	struct nfs4_sequence_args args = {};
	struct nfs4_sequence_res res = {};

	nfs4_sequence_attach_slot(&args, &res, NULL);

	KUNIT_EXPECT_PTR_EQ(test, args.sa_slot, NULL);
	KUNIT_EXPECT_PTR_EQ(test, res.sr_slot, NULL);
}

static void attach_slot_wires_up_both_args_and_res(struct kunit *test)
{
	struct nfs4_sequence_args args = { .sa_privileged = 1 };
	struct nfs4_sequence_res res = {};
	struct nfs4_slot slot = {};

	nfs4_sequence_attach_slot(&args, &res, &slot);

	KUNIT_EXPECT_PTR_EQ(test, args.sa_slot, &slot);
	KUNIT_EXPECT_PTR_EQ(test, res.sr_slot, &slot);
	KUNIT_EXPECT_TRUE_MSG(test, slot.privileged,
			      "the slot did not pick up the privileged flag");
}

/* The last renewal only ever moves forward: an older timestamp is ignored. */
static void renew_lease_only_moves_the_timestamp_forward(struct kunit *test)
{
	struct nfs_client clp = {};

	spin_lock_init(&clp.cl_lock);
	clp.cl_last_renewal = 100;

	do_renew_lease(&clp, 50);
	KUNIT_EXPECT_EQ_MSG(test, clp.cl_last_renewal, 100UL,
			    "an older timestamp moved the renewal backwards");

	do_renew_lease(&clp, 150);
	KUNIT_EXPECT_EQ(test, clp.cl_last_renewal, 150UL);
}

/*
 * NFSv4.0 has no SEQUENCE operation, so every successful call is itself a
 * lease renewal. NFSv4.1+ renews the lease as a side effect of SEQUENCE
 * instead, so renew_lease() is a no-op once a session exists.
 */
static void renew_lease_updates_the_client_without_a_session(struct kunit *test)
{
	struct nfs_client client = {};
	struct nfs_server server = {};

	spin_lock_init(&client.cl_lock);
	client.cl_session = NULL;
	server.nfs_client = &client;

	renew_lease(&server, 42);
	KUNIT_EXPECT_EQ(test, client.cl_last_renewal, 42UL);
}

static void renew_lease_is_a_no_op_once_a_session_exists(struct kunit *test)
{
	struct nfs_client client = {};
	struct nfs_server server = {};
	struct nfs4_session session = {};

	spin_lock_init(&client.cl_lock);
	client.cl_session = &session;
	server.nfs_client = &client;

	renew_lease(&server, 42);
	KUNIT_EXPECT_EQ_MSG(test, client.cl_last_renewal, 0UL,
			    "renew_lease() updated the timestamp despite a session");
}

static struct kunit_case nfs4_sequence_lease_cases[] = {
	KUNIT_CASE(init_sequence_starts_with_no_slot),
	KUNIT_CASE(attach_slot_with_no_slot_is_a_no_op),
	KUNIT_CASE(attach_slot_wires_up_both_args_and_res),
	KUNIT_CASE(renew_lease_only_moves_the_timestamp_forward),
	KUNIT_CASE(renew_lease_updates_the_client_without_a_session),
	KUNIT_CASE(renew_lease_is_a_no_op_once_a_session_exists),
	{}
};

static struct kunit_suite nfs4_sequence_lease_suite = {
	.name		= "nfs4-sequence-lease",
	.test_cases	= nfs4_sequence_lease_cases,
};

/*
 * Directory link count updates and the write path's stateid-and-cache
 * consistency helpers.
 *
 * nlink changes are NFSv4-specific plumbing: link()/unlink()/rmdir()/
 * mkdir() adjust the directory's own count as well as the target's,
 * ahead of the server's next GETATTR confirming it.
 */

static struct inode *nlink_inode(struct kunit *test, unsigned int nlink)
{
	struct bitmap_fixture *f;
	struct inode *inode;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f);

	f->rpc_ops.have_delegation = stub_have_delegation;
	f->client.rpc_ops = &f->rpc_ops;
	f->server.nfs_client = &f->client;
	f->sb.s_fs_info = &f->server;

	inode = &f->nfsi.vfs_inode;
	inode->i_sb = &f->sb;
	spin_lock_init(&inode->i_lock);
	set_nlink(inode, nlink);

	/* nfs_set_cache_invalid() reads inode->i_mapping->nrpages. */
	address_space_init_once(&f->mapping);
	f->mapping.host = inode;
	f->mapping.a_ops = &empty_aops;
	mapping_set_gfp_mask(&f->mapping, GFP_KERNEL);
	inode->i_mapping = &f->mapping;

	cur_fixture = f;
	return inode;
}

static void inc_nlink_locked_increments_and_invalidates(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);

	nfs4_inc_nlink_locked(inode);

	KUNIT_EXPECT_EQ(test, inode->i_nlink, 2U);
	KUNIT_EXPECT_TRUE(test, NFS_I(inode)->cache_validity & NFS_INO_INVALID_NLINK);
}

static void inc_nlink_takes_the_lock_itself(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);

	nfs4_inc_nlink(inode);
	KUNIT_EXPECT_EQ(test, inode->i_nlink, 2U);
}

static void dec_nlink_locked_decrements_and_invalidates(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 2);

	nfs4_dec_nlink_locked(inode);

	KUNIT_EXPECT_EQ(test, inode->i_nlink, 1U);
	KUNIT_EXPECT_TRUE(test, NFS_I(inode)->cache_validity & NFS_INO_INVALID_NLINK);
}

/*
 * The unlocked wrapper takes inode->i_lock itself around the change-info
 * update, so a caller need not hold it -- checked here by driving the same
 * scenario as an already-tested nfs4-change-info case, through the
 * spinlocked entry point instead of the _locked one directly.
 */
static void update_changeattr_takes_the_inode_lock_itself(struct kunit *test)
{
	struct reply_fixture *f = reply_dir(test, 10, S_IFDIR | 0755);
	struct nfs4_change_info cinfo = { .atomic = 1, .before = 10, .after = 11 };

	nfs4_update_changeattr(&f->nfsi.vfs_inode, &cinfo, jiffies, 0);

	KUNIT_EXPECT_EQ(test, dir_change_attr(f), 11ULL);
}

/*
 * Stateid sequencing and open/close state transitions
 */

/* The very first OPEN in a generation is recognised by seqid 1, unconditionally. */
static void first_open_in_a_generation_is_seqid_one(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid stateid = { .seqid = cpu_to_be32(1) };

	KUNIT_EXPECT_TRUE(test, nfs_stateid_is_sequential(state, &stateid));
}

/* Without NFS_OPEN_STATE set, any other seqid is rejected outright. */
static void without_open_state_only_seqid_one_is_accepted(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid stateid = { .seqid = cpu_to_be32(2) };

	KUNIT_EXPECT_FALSE(test, nfs_stateid_is_sequential(state, &stateid));
}

/* With a matching "other" field, only the very next seqid is sequential. */
static void a_matching_stateid_must_advance_by_exactly_one(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid next = { .seqid = cpu_to_be32(6) };
	nfs4_stateid skip = { .seqid = cpu_to_be32(7) };

	set_bit(NFS_OPEN_STATE, &state->flags);
	state->open_stateid.seqid = cpu_to_be32(5);
	memset(state->open_stateid.other, 0x9, sizeof(state->open_stateid.other));
	memcpy(next.other, state->open_stateid.other, sizeof(next.other));
	memcpy(skip.other, state->open_stateid.other, sizeof(skip.other));

	KUNIT_EXPECT_TRUE(test, nfs_stateid_is_sequential(state, &next));
	KUNIT_EXPECT_FALSE_MSG(test, nfs_stateid_is_sequential(state, &skip),
			       "a skipped seqid was accepted as sequential");
}

/*
 * A reply carrying a different "other" field is a brand new stateid from
 * the server -- accepted unconditionally as the start of a fresh
 * generation, seqid 1 only.
 */
static void a_stateid_with_a_different_other_field_is_a_new_generation(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid fresh = { .seqid = cpu_to_be32(1) };

	set_bit(NFS_OPEN_STATE, &state->flags);
	state->open_stateid.seqid = cpu_to_be32(5);
	memset(state->open_stateid.other, 0x9, sizeof(state->open_stateid.other));
	memset(fresh.other, 0x1, sizeof(fresh.other));

	KUNIT_EXPECT_TRUE(test, nfs_stateid_is_sequential(state, &fresh));
}

/*
 * nfs_clear_open_stateid() ignores a CLOSE reply whose stateid does not
 * match the state's current open stateid -- a race with a newer OPEN.
 * Deliberately not exercising the NFS_STATE_RECLAIM_NOGRACE path: that
 * calls nfs4_schedule_state_manager(), which starts a kernel thread.
 */
static void a_mismatched_close_reply_is_ignored(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid arg = {};

	set_bit(NFS_OPEN_STATE, &state->flags);
	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	memset(state->open_stateid.other, 0x1, sizeof(state->open_stateid.other));
	memset(arg.other, 0x2, sizeof(arg.other));

	nfs_clear_open_stateid(state, &arg, NULL, 0);

	KUNIT_EXPECT_TRUE_MSG(test, test_bit(NFS_O_RDONLY_STATE, &state->flags),
			      "a mismatched CLOSE reply still cleared the state");
}

static void a_matching_close_reply_clears_the_state(struct kunit *test)
{
	struct nfs4_state *state = open_state(test);
	nfs4_stateid arg = {};

	set_bit(NFS_OPEN_STATE, &state->flags);
	set_bit(NFS_O_RDONLY_STATE, &state->flags);
	memset(state->open_stateid.other, 0x1, sizeof(state->open_stateid.other));
	memset(arg.other, 0x1, sizeof(arg.other));

	nfs_clear_open_stateid(state, &arg, NULL, 0);

	KUNIT_EXPECT_FALSE(test, test_bit(NFS_O_RDONLY_STATE, &state->flags));
}

/* With no delegation cached, there is nothing incompatible to return. */
static void return_incompatible_delegation_with_none_cached_is_a_no_op(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);

	nfs4_return_incompatible_delegation(inode, FMODE_READ);
}

/* Closing a context that never opened anything is a no-op. */
static void close_context_with_no_open_state_is_a_no_op(struct kunit *test)
{
	struct dentry dentry = {};
	struct nfs_open_context ctx = { .dentry = &dentry, .state = NULL };

	nfs4_close_context(&ctx, 1);
	nfs4_close_context(&ctx, 0);
}

static struct kunit_case nfs4_open_close_cases[] = {
	KUNIT_CASE(inc_nlink_locked_increments_and_invalidates),
	KUNIT_CASE(inc_nlink_takes_the_lock_itself),
	KUNIT_CASE(dec_nlink_locked_decrements_and_invalidates),
	KUNIT_CASE(update_changeattr_takes_the_inode_lock_itself),
	KUNIT_CASE(first_open_in_a_generation_is_seqid_one),
	KUNIT_CASE(without_open_state_only_seqid_one_is_accepted),
	KUNIT_CASE(a_matching_stateid_must_advance_by_exactly_one),
	KUNIT_CASE(a_stateid_with_a_different_other_field_is_a_new_generation),
	KUNIT_CASE(a_mismatched_close_reply_is_ignored),
	KUNIT_CASE(a_matching_close_reply_clears_the_state),
	KUNIT_CASE(return_incompatible_delegation_with_none_cached_is_a_no_op),
	KUNIT_CASE(close_context_with_no_open_state_is_a_no_op),
	{}
};

static struct kunit_suite nfs4_open_close_suite = {
	.name		= "nfs4-open-close",
	.test_cases	= nfs4_open_close_cases,
};

/*
 * Read/write path helpers and small standalone utilities
 *
 * nfs4_read_plus_not_supported() and nfs4_write/read_stateid_changed() all
 * call rpc_restart_call_prepare() on their positive branch, which reads
 * task->tk_ops -- NULL on a bare struct rpc_task and therefore a crash
 * with the minimal fixture used here. Only the negative branches (no
 * restart needed) are exercised.
 */

static void read_plus_not_supported_ignores_a_different_procedure(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	struct nfs_pgio_header hdr = { .inode = inode };
	struct rpc_task task = { .tk_status = -ENOTSUPP };
	struct rpc_message msg = { .rpc_proc = NULL };

	task.tk_msg = msg;
	KUNIT_EXPECT_FALSE(test, nfs4_read_plus_not_supported(&task, &hdr));
}

static void read_plus_not_supported_ignores_a_different_status(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	struct nfs_pgio_header hdr = { .inode = inode };
	struct rpc_task task = { .tk_status = -EACCES };

	task.tk_msg.rpc_proc = &nfs4_procedures[NFSPROC4_CLNT_READ_PLUS];
	KUNIT_EXPECT_FALSE_MSG(test, nfs4_read_plus_not_supported(&task, &hdr),
			       "treated an unrelated error as READ_PLUS being unsupported");
}

/* pNFS and O_DIRECT writes never ask for post-write attributes. */
static void pnfs_writes_never_need_cache_consistency_data(struct kunit *test)
{
	struct nfs_pgio_header hdr = {};
	struct nfs_client dummy_ds_client = {};

	/*
	 * ds_clp only needs to be non-NULL here: nfs4_write_need_cache_
	 * consistency_data() checks the pointer, never dereferences it.
	 */
	hdr.ds_clp = &dummy_ds_client;
	KUNIT_EXPECT_FALSE(test, nfs4_write_need_cache_consistency_data(&hdr));
}

static void direct_writes_never_need_cache_consistency_data(struct kunit *test)
{
	struct nfs_pgio_header hdr = {};
	struct nfs_direct_req dreq = {};

	hdr.dreq = &dreq;
	KUNIT_EXPECT_FALSE(test, nfs4_write_need_cache_consistency_data(&hdr));
}

/* An ordinary buffered write with no delegation does need them. */
static void a_plain_write_without_a_delegation_needs_them(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	struct nfs_pgio_header hdr = { .inode = inode };

	KUNIT_EXPECT_TRUE(test, nfs4_write_need_cache_consistency_data(&hdr));
}

/*
 * Holding a delegation means the client already trusts its own cache.
 *
 * nfs4_write_need_cache_consistency_data() reaches this through
 * nfs4_have_delegation(), which reads NFS_I(inode)->delegation directly
 * rather than through the NFS_PROTO(inode) vtable the bitmap/changeattr
 * fixtures stub -- so a real (if minimal) struct nfs_delegation is needed
 * here instead of cur_fixture->deleg.
 */
static void a_plain_write_with_a_delegation_does_not_need_them(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	struct nfs_pgio_header hdr = { .inode = inode };
	struct nfs_delegation *delegation;

	delegation = kunit_kzalloc(test, sizeof(*delegation), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, delegation);
	delegation->type = FMODE_READ | FMODE_WRITE;
	rcu_assign_pointer(NFS_I(inode)->delegation, delegation);

	KUNIT_EXPECT_FALSE(test, nfs4_write_need_cache_consistency_data(&hdr));
}

/*
 * nfs4_bitmask_set() folds cache-invalidity flags into a copy of the
 * server's default bitmask, the mirror image of nfs4_bitmap_copy_adjust()
 * tested earlier: that trims a full mask down under a delegation, this
 * builds one up from what the cache is missing.
 */

static void bitmask_set_copies_the_source_when_nothing_is_invalid(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	__u32 dst[NFS_BITMASK_SZ] = {};
	__u32 src[NFS_BITMASK_SZ] = { FATTR4_WORD0_TYPE, 0, 0 };

	NFS_SERVER(inode)->attr_bitmask[0] = ~0U;
	NFS_SERVER(inode)->attr_bitmask[1] = ~0U;
	NFS_SERVER(inode)->attr_bitmask[2] = ~0U;

	nfs4_bitmask_set(dst, src, inode, 0);
	KUNIT_EXPECT_EQ(test, dst[0], (__u32)FATTR4_WORD0_TYPE);
}

static void bitmask_set_adds_bits_for_each_invalid_attribute(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	__u32 dst[NFS_BITMASK_SZ] = {};
	__u32 src[NFS_BITMASK_SZ] = {};

	NFS_SERVER(inode)->attr_bitmask[0] = ~0U;
	NFS_SERVER(inode)->attr_bitmask[1] = ~0U;
	NFS_SERVER(inode)->attr_bitmask[2] = ~0U;

	nfs4_bitmask_set(dst, src, inode,
			 NFS_INO_INVALID_CHANGE | NFS_INO_INVALID_ATIME);

	KUNIT_EXPECT_TRUE(test, dst[0] & FATTR4_WORD0_CHANGE);
	KUNIT_EXPECT_TRUE(test, dst[1] & FATTR4_WORD1_TIME_ACCESS);
	KUNIT_EXPECT_FALSE_MSG(test, dst[1] & FATTR4_WORD1_MODE,
			       "requested an attribute that was not marked invalid");
}

/*
 * Whatever cache invalidity would otherwise add, the result never asks for
 * an attribute the server does not itself advertise support for.
 */
static void bitmask_set_never_exceeds_the_servers_own_bitmask(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	__u32 dst[NFS_BITMASK_SZ] = {};
	__u32 src[NFS_BITMASK_SZ] = {};

	NFS_SERVER(inode)->attr_bitmask[0] = 0;
	NFS_SERVER(inode)->attr_bitmask[1] = 0;
	NFS_SERVER(inode)->attr_bitmask[2] = 0;

	nfs4_bitmask_set(dst, src, inode, NFS_INO_INVALID_CHANGE);

	KUNIT_EXPECT_EQ_MSG(test, dst[0], 0U,
			    "requested an attribute the server does not support");
}

/*
 * nfs4_buf_to_pages_noslab(): splitting a flat buffer across page-sized
 * chunks for the SETACL/SETXATTR wire format.
 */

static void buf_to_pages_action(void *p)
{
	struct page **pages = p;
	int i;

	for (i = 0; i < 4 && pages[i]; i++)
		__free_page(pages[i]);
}

static void a_buffer_under_one_page_uses_a_single_page(struct kunit *test)
{
	/*
	 * Allocated, not a stack array: kunit_add_action_or_reset() runs its
	 * cleanup after this function returns, so a stack local would be a
	 * dangling pointer by the time it fires.
	 */
	struct page **pages = kunit_kzalloc(test, 4 * sizeof(*pages), GFP_KERNEL);
	char buf[100];
	int rc;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pages);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, buf_to_pages_action,
							pages), 0);
	memset(buf, 0xAB, sizeof(buf));

	rc = nfs4_buf_to_pages_noslab(buf, sizeof(buf), pages);

	KUNIT_ASSERT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, memcmp(page_address(pages[0]), buf, sizeof(buf)), 0);
}

static void a_multi_page_buffer_splits_across_pages(struct kunit *test)
{
	struct page **pages = kunit_kzalloc(test, 4 * sizeof(*pages), GFP_KERNEL);
	char *buf;
	size_t len = PAGE_SIZE + 100;
	int rc;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pages);
	buf = kunit_kmalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xCD, len);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, buf_to_pages_action,
							pages), 0);

	rc = nfs4_buf_to_pages_noslab(buf, len, pages);

	KUNIT_ASSERT_EQ_MSG(test, rc, 2, "a buffer just over one page took %d pages", rc);
	KUNIT_EXPECT_EQ(test, memcmp(page_address(pages[0]), buf, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, memcmp(page_address(pages[1]), buf + PAGE_SIZE, 100), 0);
}

/*
 * Small standalone utilities
 */

/*
 * The cached ACL is discarded by handing the setter a NULL replacement,
 * which kfree()s whatever was cached before. The stand-in has to be a
 * real heap allocation for that reason -- a stack local here would be an
 * invalid free.
 */
static void zap_acl_attr_discards_the_cached_acl(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);

	NFS_I(inode)->nfs4_acl = kmalloc(8, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, NFS_I(inode)->nfs4_acl);

	nfs4_zap_acl_attr(inode);

	KUNIT_EXPECT_PTR_EQ(test, NFS_I(inode)->nfs4_acl, NULL);
}

/*
 * A SECINFO reply carries no real attributes, so the client fabricates
 * enough of them to treat the result as a directory: this is what lets
 * "cd" into a pseudo-fs security-negotiation node work at all.
 */
static void secinfo_attributes_are_fabricated_as_a_directory(struct kunit *test)
{
	struct nfs_fattr fattr = {};

	nfs_fixup_secinfo_attributes(&fattr);

	KUNIT_EXPECT_TRUE(test, fattr.valid & NFS_ATTR_FATTR_TYPE);
	KUNIT_EXPECT_TRUE(test, S_ISDIR(fattr.mode));
	KUNIT_EXPECT_EQ(test, fattr.nlink, 2U);
}

/* Disabling swap flags the state manager to exit once it next wakes. */
static void disable_swap_flags_the_manager_to_exit(struct kunit *test)
{
	struct inode *inode = nlink_inode(test, 1);
	struct nfs_client *clp = NFS_SERVER(inode)->nfs_client;

	set_bit(NFS4CLNT_MANAGER_AVAILABLE, &clp->cl_state);

	nfs4_disable_swap(inode);

	KUNIT_EXPECT_TRUE(test, test_bit(NFS4CLNT_RUN_MANAGER, &clp->cl_state));
	KUNIT_EXPECT_FALSE(test, test_bit(NFS4CLNT_MANAGER_AVAILABLE, &clp->cl_state));
}

/*
 * NFS4CLNT_PURGE_STATE forces an impossible boot verifier so the server
 * can never mistake this mount for one continuing across a client
 * restart -- every OPEN then looks like a fresh client to the server,
 * forcing it to discard any state left over from before the purge.
 *
 * The non-purge branch reads the network namespace's boot time via
 * net_generic(), which is not exercised here.
 */
static void purge_state_forces_an_impossible_boot_verifier(struct kunit *test)
{
	struct nfs_client clp = {};
	nfs4_verifier verf;
	__be32 all_ones[2] = { cpu_to_be32(U32_MAX), cpu_to_be32(U32_MAX) };

	set_bit(NFS4CLNT_PURGE_STATE, &clp.cl_state);
	nfs4_init_boot_verifier(&clp, &verf);

	KUNIT_EXPECT_EQ(test, memcmp(verf.data, all_ones, sizeof(all_ones)), 0);
}

static struct kunit_case nfs4_read_write_helper_cases[] = {
	KUNIT_CASE(read_plus_not_supported_ignores_a_different_procedure),
	KUNIT_CASE(read_plus_not_supported_ignores_a_different_status),
	KUNIT_CASE(pnfs_writes_never_need_cache_consistency_data),
	KUNIT_CASE(direct_writes_never_need_cache_consistency_data),
	KUNIT_CASE(a_plain_write_without_a_delegation_needs_them),
	KUNIT_CASE(a_plain_write_with_a_delegation_does_not_need_them),
	KUNIT_CASE(bitmask_set_copies_the_source_when_nothing_is_invalid),
	KUNIT_CASE(bitmask_set_adds_bits_for_each_invalid_attribute),
	KUNIT_CASE(bitmask_set_never_exceeds_the_servers_own_bitmask),
	KUNIT_CASE(a_buffer_under_one_page_uses_a_single_page),
	KUNIT_CASE(a_multi_page_buffer_splits_across_pages),
	KUNIT_CASE(zap_acl_attr_discards_the_cached_acl),
	KUNIT_CASE(secinfo_attributes_are_fabricated_as_a_directory),
	KUNIT_CASE(disable_swap_flags_the_manager_to_exit),
	KUNIT_CASE(purge_state_forces_an_impossible_boot_verifier),
	{}
};

static struct kunit_suite nfs4_read_write_helper_suite = {
	.name		= "nfs4-read-write-helpers",
	.test_cases	= nfs4_read_write_helper_cases,
};

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

static struct kunit_case nfs4_acl_cases[] = {
	KUNIT_CASE(acl_support_follows_the_attribute_bitmask),
	KUNIT_CASE(acl_flavours_are_advertised_independently),
	KUNIT_CASE(get_acl_on_an_empty_filehandle_reports_no_data),
	KUNIT_CASE(set_acl_on_an_empty_filehandle_reports_no_data),
	KUNIT_CASE(the_filehandle_check_precedes_the_support_check),
	KUNIT_CASE(get_acl_for_an_unsupported_flavour_is_refused),
	KUNIT_CASE(set_acl_for_an_unsupported_flavour_is_refused),
	KUNIT_CASE(setting_an_empty_acl_is_rejected),
	KUNIT_CASE(the_empty_check_precedes_the_support_check),
	KUNIT_CASE(an_oversized_acl_is_refused),
	{}
};

static struct kunit_suite nfs4_acl_suite = {
	.name		= "nfs4-acl-gates",
	.test_cases	= nfs4_acl_cases,
};

static struct kunit_case nfs4_changeattr_cases[] = {
	KUNIT_CASE(an_atomic_change_keeps_cached_lookups),
	KUNIT_CASE(a_non_atomic_change_drops_cached_lookups),
	KUNIT_CASE(an_atomic_change_from_an_unexpected_state_drops_lookups),
	KUNIT_CASE(a_non_atomic_change_invalidates_the_attribute_cache),
	KUNIT_CASE(an_atomic_change_spares_the_attribute_cache),
	KUNIT_CASE(a_stale_reply_does_not_rewind_the_change_attribute),
	KUNIT_CASE(an_undefined_change_type_accepts_any_different_value),
	KUNIT_CASE(an_unchanged_directory_is_left_alone),
	KUNIT_CASE(a_regular_file_never_forces_lookup_revalidation),
	KUNIT_CASE(a_directory_always_invalidates_its_contents),
	KUNIT_CASE(an_empty_page_cache_drops_the_data_invalidation),
	KUNIT_CASE(a_delegation_spares_the_timestamps),
	KUNIT_CASE(without_a_delegation_the_timestamps_are_invalidated),
	{}
};

static struct kunit_suite nfs4_changeattr_suite = {
	.name		= "nfs4-change-info",
	.test_cases	= nfs4_changeattr_cases,
};

static struct kunit_case nfs4_unlink_done_cases[] = {
	KUNIT_CASE(a_successful_unlink_reply_updates_the_directory),
	KUNIT_CASE(a_non_atomic_unlink_reply_drops_cached_lookups),
	KUNIT_CASE(an_atomic_unlink_reply_keeps_cached_lookups),
	KUNIT_CASE(a_failed_unlink_reply_leaves_the_directory_alone),
	{}
};

static struct kunit_suite nfs4_unlink_done_suite = {
	.name		= "nfs4-unlink-done",
	.test_cases	= nfs4_unlink_done_cases,
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
		  &nfs4_bitmap_suite,
		  &nfs4_acl_suite,
		  &nfs4_changeattr_suite,
		  &nfs4_unlink_done_suite,
		  &nfs4_misc_suite,
		  &nfs4_open_state_suite,
		  &nfs4_session_negotiation_suite,
		  &nfs4_stateid_status_suite,
		  &nfs4_sequence_lease_suite,
		  &nfs4_open_close_suite,
		  &nfs4_read_write_helper_suite);

MODULE_DESCRIPTION("Test NFSv4 protocol decision logic");
MODULE_LICENSE("GPL");
