// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for NFS mount option parsing in fs/nfs/fs_context.c.
 *
 * nfs_fs_context_parse_param() is a 400-line switch that turns one mount
 * option into bits in a struct nfs_fs_context. It issues no RPCs, takes no
 * locks and touches no inodes, yet almost none of it is exercised by a
 * mount: a run only ever parses the handful of options that run used.
 *
 * It is also the widest exposed surface the client has. Every option here
 * comes from userspace, several of them steal the parsed string rather than
 * copy it, and the bounds checks are the only thing between a typo and an
 * out-of-range rsize or port. A wrong bit set here changes what the client
 * asks the server for, which xfstests cannot see: a mount that silently
 * lands on NFS_MOUNT_SOFT instead of NFS_MOUNT_SOFTERR still reads and
 * writes files correctly, it just fails differently when the server stops
 * answering.
 *
 * The seam is that fs_parse() only uses the fs_context for logging and
 * nfs_fc2context() is a plain fc->fs_private dereference, so a zeroed
 * fs_context on the stack with fs_private pointed at a context is a
 * complete parsing environment. nfs_invalf() (fs/nfs/internal.h) checks
 * fc->log.log before calling invalf(), so leaving the log NULL routes
 * rejections through dprintk() and still returns -EINVAL.
 */

#include <kunit/test.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/kernel.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>
#include <linux/nfs4_mount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/sunrpc/xprt.h>
#include <net/net_namespace.h>

#include "internal.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Private to fs_context.c; un-staticed by scripts/kunit/run-nfs-kunit.sh. */
int nfs_fs_context_parse_param(struct fs_context *fc,
			       struct fs_parameter *param);
int nfs_validate_transport_protocol(struct fs_context *fc,
				    struct nfs_fs_context *ctx);
void nfs_set_mount_transport_protocol(struct nfs_fs_context *ctx);

/*
 * The fs_context and the nfs_fs_context it points at, allocated together
 * so one kunit_kzalloc() covers both and the parse target is always the
 * zeroed state a fresh mount starts from.
 */
struct mount_ctx {
	struct fs_context	fc;
	struct nfs_fs_context	ctx;
};

/*
 * Options that take ownership of param->string leave a kmalloc'd pointer
 * behind in the context. kunit_kzalloc() frees the structs, not what they
 * point at, so the strings are freed here.
 */
static void mount_ctx_release(void *p)
{
	struct mount_ctx *m = p;

	kfree(m->ctx.fscache_uniq);
	kfree(m->ctx.client_address);
	kfree(m->ctx.mount_server.hostname);
	kfree(m->ctx.nfs_server.hostname);
	kfree(m->ctx.nfs_server.export_path);
	kfree(m->fc.source);
}

static struct mount_ctx *mount_ctx_new(struct kunit *test)
{
	struct mount_ctx *m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, m);

	/*
	 * addr=/mountaddr= pass fc->net_ns to rpc_pton(), and
	 * fatal_neterrors=default compares it against init_net. A borrowed
	 * pointer is enough; nothing here takes or drops a reference.
	 */
	m->fc.net_ns = &init_net;
	m->fc.fs_private = &m->ctx;

	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, mount_ctx_release, m), 0);
	return m;
}

/* Parse one valueless option, e.g. "hard" or its negation "nolock". */
static int parse_flag(struct mount_ctx *m, const char *key)
{
	struct fs_parameter param = {
		.key	= key,
		.type	= fs_value_is_flag,
	};

	return nfs_fs_context_parse_param(&m->fc, &param);
}

/*
 * Parse one "key=value" option. The value has to be kmalloc'd because
 * Opt_source, Opt_fscache, Opt_clientaddr and Opt_mounthost take ownership
 * of param->string and NULL it out; whatever is left afterwards is still
 * ours.
 */
static int parse_value(struct kunit *test, struct mount_ctx *m,
		       const char *key, const char *value)
{
	struct fs_parameter param = {
		.key	= key,
		.type	= fs_value_is_string,
		.string	= kstrdup(value, GFP_KERNEL),
		.size	= strlen(value),
	};
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, param.string);
	ret = nfs_fs_context_parse_param(&m->fc, &param);
	kfree(param.string);
	return ret;
}

/*
 * Flag options: foo and, where the spec says fs_param_neg_with_no, nofoo.
 *
 * Several of these clear bits as well as set them, so each case names the
 * whole ctx->flags word before and after rather than one bit. That is what
 * pins, for instance, soft dropping NFS_MOUNT_SOFTERR on its way in.
 */

struct flag_param {
	const char	*desc;
	const char	*key;
	unsigned int	before;
	unsigned int	after;
};

static void flag_get_desc(const struct flag_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct flag_param flag_params[] = {
	{ "soft sets SOFT", "soft", 0, NFS_MOUNT_SOFT },
	{
		.desc	= "soft clears SOFTERR",
		.key	= "soft",
		.before	= NFS_MOUNT_SOFTERR | NFS_MOUNT_SOFTREVAL,
		.after	= NFS_MOUNT_SOFT | NFS_MOUNT_SOFTREVAL,
	},
	{
		/* softerr implies softreval, and is exclusive with soft */
		.desc	= "softerr sets SOFTERR and SOFTREVAL",
		.key	= "softerr",
		.before	= NFS_MOUNT_SOFT,
		.after	= NFS_MOUNT_SOFTERR | NFS_MOUNT_SOFTREVAL,
	},
	{
		.desc	= "hard clears all three soft bits",
		.key	= "hard",
		.before	= NFS_MOUNT_SOFT | NFS_MOUNT_SOFTERR |
			  NFS_MOUNT_SOFTREVAL,
		.after	= 0,
	},
	{ "softreval sets SOFTREVAL", "softreval", 0, NFS_MOUNT_SOFTREVAL },
	{ "posix sets POSIX", "posix", 0, NFS_MOUNT_POSIX },
	{ "noposix clears POSIX", "noposix", NFS_MOUNT_POSIX, 0 },
	/* cto is stored inverted: the flag is NOCTO */
	{ "cto clears NOCTO", "cto", NFS_MOUNT_NOCTO, 0 },
	{ "nocto sets NOCTO", "nocto", 0, NFS_MOUNT_NOCTO },
	{ "ac clears NOAC", "ac", NFS_MOUNT_NOAC, 0 },
	{ "noac sets NOAC", "noac", 0, NFS_MOUNT_NOAC },
	{ "acl clears NOACL", "acl", NFS_MOUNT_NOACL, 0 },
	{ "noacl sets NOACL", "noacl", 0, NFS_MOUNT_NOACL },
	/* alignwrite is stored inverted too */
	{ "alignwrite clears NO_ALIGNWRITE", "alignwrite",
	  NFS_MOUNT_NO_ALIGNWRITE, 0 },
	{ "noalignwrite sets NO_ALIGNWRITE", "noalignwrite", 0,
	  NFS_MOUNT_NO_ALIGNWRITE },
	{ "trunkdiscovery sets TRUNK_DISCOVERY", "trunkdiscovery", 0,
	  NFS_MOUNT_TRUNK_DISCOVERY },
	{ "notrunkdiscovery clears TRUNK_DISCOVERY", "notrunkdiscovery",
	  NFS_MOUNT_TRUNK_DISCOVERY, 0 },
	{ "sharecache clears UNSHARED", "sharecache", NFS_MOUNT_UNSHARED, 0 },
	{ "nosharecache sets UNSHARED", "nosharecache", 0,
	  NFS_MOUNT_UNSHARED },
	{ "resvport clears NORESVPORT", "resvport", NFS_MOUNT_NORESVPORT, 0 },
	{ "noresvport sets NORESVPORT", "noresvport", 0,
	  NFS_MOUNT_NORESVPORT },
	{
		/* rdirplus with no value clears both of the rdirplus bits */
		.desc	= "rdirplus clears NORDIRPLUS and FORCE_RDIRPLUS",
		.key	= "rdirplus",
		.before	= NFS_MOUNT_NORDIRPLUS | NFS_MOUNT_FORCE_RDIRPLUS,
		.after	= 0,
	},
	{
		.desc	= "nordirplus sets NORDIRPLUS and clears FORCE",
		.key	= "nordirplus",
		.before	= NFS_MOUNT_FORCE_RDIRPLUS,
		.after	= NFS_MOUNT_NORDIRPLUS,
	},
	{
		.desc	= "nolock sets NONLM and both local lock bits",
		.key	= "nolock",
		.before	= 0,
		.after	= NFS_MOUNT_NONLM | NFS_MOUNT_LOCAL_FLOCK |
			  NFS_MOUNT_LOCAL_FCNTL,
	},
	{
		.desc	= "lock clears NONLM and both local lock bits",
		.key	= "lock",
		.before	= NFS_MOUNT_NONLM | NFS_MOUNT_LOCAL_FLOCK |
			  NFS_MOUNT_LOCAL_FCNTL,
		.after	= 0,
	},
};

KUNIT_ARRAY_PARAM(flag, flag_params, flag_get_desc);

static void flag_case(struct kunit *test)
{
	const struct flag_param *param = test->param_value;
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.flags = param->before;

	KUNIT_EXPECT_EQ(test, parse_flag(m, param->key), 0);
	KUNIT_EXPECT_EQ_MSG(test, m->ctx.flags, param->after,
			    "option %s", param->key);
}

/*
 * softreval is declared fsparam_flag, not fsparam_flag_no, so there is no
 * nosoftreval spelling to strip the bit again: the option is not
 * recognised at all. That makes the result.negated branch under
 * case Opt_softreval unreachable through this parameter table, unlike the
 * negated branches of every other option that has one.
 */
static void softreval_has_no_negated_spelling(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.flags = NFS_MOUNT_SOFTREVAL;

	KUNIT_EXPECT_EQ(test, parse_flag(m, "nosoftreval"), -ENOPARAM);
	KUNIT_EXPECT_EQ(test, m->ctx.flags, NFS_MOUNT_SOFTREVAL);
}

/* lock=/nolock= also record which one the user asked for, for validation. */
static void lock_records_the_requested_state(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, m->ctx.lock_status, NFS_LOCK_NOT_SET);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "nolock"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.lock_status, NFS_LOCK_NOLOCK);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "lock"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.lock_status, NFS_LOCK_LOCK);
}

static void sloppy_is_recorded(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_FALSE(test, m->ctx.sloppy);
	KUNIT_EXPECT_EQ(test, parse_flag(m, "sloppy"), 0);
	KUNIT_EXPECT_TRUE(test, m->ctx.sloppy);
}

/*
 * An option the table does not list is -ENOPARAM, which the caller turns
 * into a mount failure. Under sloppy the same option returns 1, which is
 * "parsed, ignore me" rather than 0.
 */
static void an_unknown_option_is_rejected(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "nosuchoption"), -ENOPARAM);
}

static void sloppy_swallows_an_unknown_option(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.sloppy = true;
	KUNIT_EXPECT_EQ(test, parse_flag(m, "nosuchoption"), 1);
}

/* A value handed to an option that takes none is a parse error. */
static void a_flag_option_rejects_a_value(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "hard", "yes"), -EINVAL);
}

/*
 * Numeric options.
 */

static void size_options_are_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "rsize", "8192"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.rsize, 8192u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "wsize", "16384"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.wsize, 16384u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "bsize", "4096"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.bsize, 4096u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "namlen", "255"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.namlen, 255u);
}

/* rsize/wsize/bsize have no bounds check here; clamping happens later. */
static void size_options_are_not_bounded_at_parse_time(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "rsize", "4294967295"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.rsize, 4294967295u);
}

static void timeout_options_are_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "timeo", "600"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.timeo, 600u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "retrans", "2"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.retrans, 2u);
}

static void attribute_cache_options_are_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "acregmin", "3"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.acregmin, 3u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "acregmax", "60"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.acregmax, 60u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "acdirmin", "30"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.acdirmin, 30u);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "acdirmax", "90"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.acdirmax, 90u);
}

/* actimeo is shorthand: it writes all four attribute cache timeouts. */
static void actimeo_sets_all_four_timeouts(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "actimeo", "45"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.acregmin, 45u);
	KUNIT_EXPECT_EQ(test, m->ctx.acregmax, 45u);
	KUNIT_EXPECT_EQ(test, m->ctx.acdirmin, 45u);
	KUNIT_EXPECT_EQ(test, m->ctx.acdirmax, 45u);
}

static void port_options_are_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "port", "2049"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.port, 2049);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "mountport", "635"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.mount_server.port, 635);
}

/* Port 0 means "ask rpcbind", so it is accepted rather than rejected. */
static void port_zero_is_accepted(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "port", "0"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.port, 0);
}

static void connection_count_options_are_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "nconnect", "4"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.nconnect, 4);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "max_connect", "8"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.max_connect, 8);
}

static void mount_protocol_version_is_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "mountvers", "3"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.mount_server.version, 3u);
}

static void minorversion_is_stored(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	/*
	 * 1 is inside the accepted range on every config: the bounds are
	 * NFS4_MIN_MINOR_VERSION (0 or 1) and NFS4_MAX_MINOR_VERSION
	 * (1 or 2) depending on which NFSv4 minor versions are built.
	 */
	KUNIT_EXPECT_EQ(test, parse_value(test, m, "minorversion", "1"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.minorversion, 1u);
}

/*
 * Out-of-range and unparsable values. Every one of these ends at an
 * nfs_invalf() and comes back as -EINVAL.
 */

struct reject_param {
	const char	*desc;
	const char	*key;
	const char	*value;
};

static void reject_get_desc(const struct reject_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct reject_param reject_params[] = {
	/* ports are u16 on the wire */
	{ "port above 65535", "port", "65536" },
	{ "mountport above 65535", "mountport", "65536" },
	/* timeo is in deciseconds and must be positive */
	{ "timeo of zero", "timeo", "0" },
	{ "timeo above INT_MAX", "timeo", "2147483648" },
	{ "retrans above INT_MAX", "retrans", "2147483648" },
	/* nconnect and max_connect are 1..16 */
	{ "nconnect of zero", "nconnect", "0" },
	{ "nconnect above 16", "nconnect", "17" },
	{ "max_connect of zero", "max_connect", "0" },
	{ "max_connect above 16", "max_connect", "17" },
	/* the MOUNT protocol only ever had versions 1 to 3 */
	{ "mountvers below 1", "mountvers", "0" },
	{ "mountvers above 3", "mountvers", "4" },
	{ "minorversion out of range", "minorversion", "99" },
	/* not a number at all: rejected by fs_parse(), not by the switch */
	{ "non-numeric rsize", "rsize", "lots" },
	{ "negative rsize", "rsize", "-1" },
	{ "empty rsize", "rsize", "" },
	/* unrecognised values for the options that take a fixed set */
	{ "unknown vers", "vers", "5" },
	{ "unknown proto", "proto", "sctp" },
	{ "unknown mountproto", "mountproto", "sctp" },
	{ "rdma is not a mount transport", "mountproto", "rdma" },
	{ "unknown sec flavour", "sec", "krb6" },
	{ "unknown xprtsec policy", "xprtsec", "ssl" },
	{ "unknown lookupcache mode", "lookupcache", "some" },
	{ "unknown local_lock mode", "local_lock", "some" },
	{ "unknown write mode", "write", "soon" },
	{ "unknown fatal_neterrors set", "fatal_neterrors", "EIO" },
	{ "unknown rdirplus mode", "rdirplus", "maybe" },
	/* an address that is not an address */
	{ "malformed addr", "addr", "not.an.address" },
	{ "malformed mountaddr", "mountaddr", "not.an.address" },
};

KUNIT_ARRAY_PARAM(reject, reject_params, reject_get_desc);

static void reject_case(struct kunit *test)
{
	const struct reject_param *param = test->param_value;
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ_MSG(test,
			    parse_value(test, m, param->key, param->value),
			    -EINVAL, "%s=%s", param->key, param->value);
}

/*
 * Version selection. vers= and nfsvers= share a parser with the bare v2,
 * v3, v4, v4.0, v4.1 and v4.2 flags, which pass their own key with the
 * leading 'v' skipped.
 */

struct version_param {
	const char	*desc;
	const char	*key;		/* NULL for the bare flag form */
	const char	*value;
	unsigned int	version;
	unsigned int	minorversion;
	bool		ver3;
};

static void version_get_desc(const struct version_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct version_param version_params[] = {
	{ "vers=2", "vers", "2", 2, 0, false },
	{ "vers=3 sets VER3", "vers", "3", 3, 0, true },
	/*
	 * Bare vers=4 is the backward compatible spelling: it picks the
	 * version but leaves the minor version alone for the server
	 * negotiation to settle.
	 */
	{ "vers=4 leaves minorversion alone", "vers", "4", 4, 0, false },
	{ "vers=4.0", "vers", "4.0", 4, 0, false },
	{ "vers=4.1", "vers", "4.1", 4, 1, false },
	{ "vers=4.2", "vers", "4.2", 4, 2, false },
	{ "nfsvers=3 sets VER3", "nfsvers", "3", 3, 0, true },
	{ "nfsvers=4.2", "nfsvers", "4.2", 4, 2, false },
	{ "v2 flag", NULL, "v2", 2, 0, false },
	{ "v3 flag sets VER3", NULL, "v3", 3, 0, true },
	{ "v4 flag", NULL, "v4", 4, 0, false },
	{ "v4.0 flag", NULL, "v4.0", 4, 0, false },
	{ "v4.1 flag", NULL, "v4.1", 4, 1, false },
	{ "v4.2 flag", NULL, "v4.2", 4, 2, false },
};

KUNIT_ARRAY_PARAM(version, version_params, version_get_desc);

static void version_case(struct kunit *test)
{
	const struct version_param *param = test->param_value;
	struct mount_ctx *m = mount_ctx_new(test);
	int ret;

	if (param->key)
		ret = parse_value(test, m, param->key, param->value);
	else
		ret = parse_flag(m, param->value);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ_MSG(test, m->ctx.version, param->version,
			    "option %s", param->value);
	KUNIT_EXPECT_EQ_MSG(test, m->ctx.minorversion, param->minorversion,
			    "option %s", param->value);
	KUNIT_EXPECT_EQ_MSG(test, !!(m->ctx.flags & NFS_MOUNT_VER3),
			    param->ver3, "option %s", param->value);
}

/*
 * Every version string clears NFS_MOUNT_VER3 before deciding, so moving
 * from v3 to v4 does not leave the v3 bit behind.
 */
static void selecting_v4_after_v3_clears_the_v3_bit(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "vers", "3"), 0);
	KUNIT_EXPECT_TRUE(test, m->ctx.flags & NFS_MOUNT_VER3);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "vers", "4.2"), 0);
	KUNIT_EXPECT_FALSE(test, m->ctx.flags & NFS_MOUNT_VER3);
}

/*
 * Security flavours. sec= takes a colon separated list and appends each
 * one to ctx->auth_info in the order given.
 */

static void sec_maps_each_name_to_its_pseudoflavor(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test,
			parse_value(test, m, "sec",
				    "sys:none:krb5:krb5i:krb5p:lkey:lkeyi:lkeyp:spkm3:spkm3i:spkm3p"),
			0);

	KUNIT_ASSERT_EQ(test, m->ctx.auth_info.flavor_len, 11u);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[0], RPC_AUTH_UNIX);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[1], RPC_AUTH_NULL);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[2], RPC_AUTH_GSS_KRB5);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[3], RPC_AUTH_GSS_KRB5I);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[4], RPC_AUTH_GSS_KRB5P);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[5], RPC_AUTH_GSS_LKEY);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[6], RPC_AUTH_GSS_LKEYI);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[7], RPC_AUTH_GSS_LKEYP);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[8], RPC_AUTH_GSS_SPKM);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[9], RPC_AUTH_GSS_SPKMI);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[10], RPC_AUTH_GSS_SPKMP);
}

/* "null" is an accepted spelling of "none". */
static void sec_null_is_the_same_as_none(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "sec", "null"), 0);
	KUNIT_ASSERT_EQ(test, m->ctx.auth_info.flavor_len, 1u);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[0], RPC_AUTH_NULL);
}

/* A flavour already in the list is not added twice. */
static void sec_ignores_a_repeated_flavor(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "sec", "sys:krb5:sys"), 0);
	KUNIT_ASSERT_EQ(test, m->ctx.auth_info.flavor_len, 2u);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[0], RPC_AUTH_UNIX);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[1], RPC_AUTH_GSS_KRB5);
}

/*
 * strsep() hands back an empty string for a leading, trailing or doubled
 * separator, and the loop skips those rather than failing the mount.
 */
static void sec_skips_empty_list_entries(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "sec", ":sys::krb5:"), 0);
	KUNIT_ASSERT_EQ(test, m->ctx.auth_info.flavor_len, 2u);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[0], RPC_AUTH_UNIX);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavors[1], RPC_AUTH_GSS_KRB5);
}

/*
 * A bad name aborts the whole list, so flavours before it are already in
 * the context when the mount fails.
 */
static void sec_stops_at_the_first_bad_name(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "sec", "sys:krb6:krb5"),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, m->ctx.auth_info.flavor_len, 1u);
}

/*
 * Transport selection. proto= picks the NFS transport and records the
 * address family the name implies; mountproto= does the same for the
 * side MOUNT protocol, which has no RDMA spelling.
 */

static void proto_tcp_selects_tcp(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "proto", "tcp"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_TCP);
	KUNIT_EXPECT_EQ(test, m->ctx.protofamily, AF_INET);
	KUNIT_EXPECT_TRUE(test, m->ctx.flags & NFS_MOUNT_TCP);
}

static void proto_tcp6_selects_tcp_over_ipv6(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "proto", "tcp6"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_TCP);
	KUNIT_EXPECT_EQ(test, m->ctx.protofamily, AF_INET6);
}

static void proto_udp_selects_udp_and_clears_the_tcp_flag(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.flags = NFS_MOUNT_TCP;

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "proto", "udp"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_UDP);
	KUNIT_EXPECT_EQ(test, m->ctx.protofamily, AF_INET);
	KUNIT_EXPECT_FALSE(test, m->ctx.flags & NFS_MOUNT_TCP);
}

static void proto_udp6_selects_udp_over_ipv6(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "proto", "udp6"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_UDP);
	KUNIT_EXPECT_EQ(test, m->ctx.protofamily, AF_INET6);
}

/* The bare udp flag is the same decision without an address family. */
static void the_udp_flag_selects_udp(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.flags = NFS_MOUNT_TCP;

	KUNIT_EXPECT_EQ(test, parse_flag(m, "udp"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_UDP);
	KUNIT_EXPECT_FALSE(test, m->ctx.flags & NFS_MOUNT_TCP);
}

/*
 * The bare tcp flag resolves its own key through the transport registry
 * rather than hard-coding the ident.
 */
static void the_tcp_flag_selects_tcp(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "tcp"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_TCP);
	KUNIT_EXPECT_TRUE(test, m->ctx.flags & NFS_MOUNT_TCP);
}

static void mountproto_sets_the_side_protocol(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "mountproto", "udp"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.mount_server.protocol,
			XPRT_TRANSPORT_UDP);
	KUNIT_EXPECT_EQ(test, m->ctx.mountfamily, AF_INET);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "mountproto", "tcp6"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.mount_server.protocol,
			XPRT_TRANSPORT_TCP);
	KUNIT_EXPECT_EQ(test, m->ctx.mountfamily, AF_INET6);
}

/*
 * nfs_set_mount_transport_protocol() gives the MOUNT protocol a transport
 * derived from the NFS one, but only when mountproto= did not already
 * pick one.
 */
static void mount_transport_follows_the_nfs_transport(struct kunit *test)
{
	struct nfs_fs_context ctx = {};

	ctx.nfs_server.protocol = XPRT_TRANSPORT_UDP;
	nfs_set_mount_transport_protocol(&ctx);
	KUNIT_EXPECT_EQ(test, ctx.mount_server.protocol, XPRT_TRANSPORT_UDP);

	memset(&ctx, 0, sizeof(ctx));
	ctx.nfs_server.protocol = XPRT_TRANSPORT_TCP;
	nfs_set_mount_transport_protocol(&ctx);
	KUNIT_EXPECT_EQ(test, ctx.mount_server.protocol, XPRT_TRANSPORT_TCP);

	/* RDMA has no MOUNT spelling, so the side protocol falls back to TCP */
	memset(&ctx, 0, sizeof(ctx));
	ctx.nfs_server.protocol = XPRT_TRANSPORT_RDMA;
	nfs_set_mount_transport_protocol(&ctx);
	KUNIT_EXPECT_EQ(test, ctx.mount_server.protocol, XPRT_TRANSPORT_TCP);
}

static void an_explicit_mount_transport_is_left_alone(struct kunit *test)
{
	struct nfs_fs_context ctx = {};

	ctx.nfs_server.protocol = XPRT_TRANSPORT_TCP;
	ctx.mount_server.protocol = XPRT_TRANSPORT_UDP;
	nfs_set_mount_transport_protocol(&ctx);
	KUNIT_EXPECT_EQ(test, ctx.mount_server.protocol, XPRT_TRANSPORT_UDP);
}

/*
 * nfs_validate_transport_protocol() runs after all the options are in. It
 * defaults anything unrecognised to TCP, rejects UDP where it cannot work,
 * and promotes TCP to TCP-with-TLS when a transport security policy is set.
 */

static void an_unset_transport_defaults_to_tcp(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_TCP);
}

static void tcp_and_rdma_are_left_as_they_are(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_TCP;
	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_TCP);

	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_RDMA;
	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_RDMA);
}

/* NFSv4 has no UDP binding, so UDP is refused there on any config. */
static void udp_is_rejected_for_nfsv4(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.version = 4;
	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_UDP;

	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			-EINVAL);
}

/*
 * For NFSv2/v3 the answer is a build-time decision:
 * nfs_server_transport_udp_invalid() has two definitions, and under
 * CONFIG_NFS_DISABLE_UDP_SUPPORT the one that refuses UDP outright wins.
 * The kunit config here sets that symbol, so this test would silently
 * cover nothing if it assumed either answer.
 */
static void udp_for_nfsv3_follows_the_build_config(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);
	int expected = IS_ENABLED(CONFIG_NFS_DISABLE_UDP_SUPPORT) ? -EINVAL : 0;

	m->ctx.version = 3;
	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_UDP;

	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			expected);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol, XPRT_TRANSPORT_UDP);
}

static void a_tls_policy_promotes_tcp_to_tcp_tls(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_TCP;
	m->ctx.xprtsec.policy = RPC_XPRTSEC_TLS_ANON;

	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.protocol,
			XPRT_TRANSPORT_TCP_TLS);
}

/* Only TCP carries TLS, so a policy on any other transport is an error. */
static void a_tls_policy_on_rdma_is_rejected(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.nfs_server.protocol = XPRT_TRANSPORT_RDMA;
	m->ctx.xprtsec.policy = RPC_XPRTSEC_TLS_ANON;

	KUNIT_EXPECT_EQ(test, nfs_validate_transport_protocol(&m->fc, &m->ctx),
			-EINVAL);
}

static void xprtsec_maps_each_policy_name(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "xprtsec", "none"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.xprtsec.policy, RPC_XPRTSEC_NONE);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "xprtsec", "tls"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.xprtsec.policy, RPC_XPRTSEC_TLS_ANON);

	KUNIT_EXPECT_EQ(test, parse_value(test, m, "xprtsec", "mtls"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.xprtsec.policy, RPC_XPRTSEC_TLS_X509);
}

/*
 * Addresses.
 */

static void addr_parses_an_ipv4_address(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);
	struct sockaddr_in *sin;

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "addr", "127.0.0.1"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.addrlen,
			sizeof(struct sockaddr_in));

	sin = (struct sockaddr_in *)&m->ctx.nfs_server.address;
	KUNIT_EXPECT_EQ(test, sin->sin_family, AF_INET);
	KUNIT_EXPECT_EQ(test, sin->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
}

static void addr_parses_an_ipv6_address(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);
	struct sockaddr_in6 *sin6;

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "addr", "::1"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.nfs_server.addrlen,
			sizeof(struct sockaddr_in6));

	sin6 = (struct sockaddr_in6 *)&m->ctx.nfs_server.address;
	KUNIT_EXPECT_EQ(test, sin6->sin6_family, AF_INET6);
	KUNIT_EXPECT_TRUE(test, ipv6_addr_loopback(&sin6->sin6_addr));
}

static void mountaddr_parses_into_the_mount_server(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);
	struct sockaddr_in *sin;

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "mountaddr", "10.0.0.1"), 0);
	KUNIT_EXPECT_EQ(test, m->ctx.mount_server.addrlen,
			sizeof(struct sockaddr_in));

	sin = (struct sockaddr_in *)&m->ctx.mount_server.address;
	KUNIT_EXPECT_EQ(test, sin->sin_family, AF_INET);
	KUNIT_EXPECT_EQ(test, sin->sin_addr.s_addr, htonl(0x0a000001));
}

/*
 * Options that take a string and keep it.
 */

static void source_takes_ownership_of_the_string(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test,
			parse_value(test, m, "source", "server:/export"), 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, m->fc.source);
	KUNIT_EXPECT_STREQ(test, m->fc.source, "server:/export");
}

/* fc->source is set once; a second source= is a mount error. */
static void a_second_source_is_rejected(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "source", "a:/one"), 0);
	KUNIT_EXPECT_EQ(test, parse_value(test, m, "source", "b:/two"),
			-EINVAL);
	KUNIT_EXPECT_STREQ(test, m->fc.source, "a:/one");
}

static void clientaddr_and_mounthost_are_kept(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test,
			parse_value(test, m, "clientaddr", "192.168.0.2"), 0);
	KUNIT_EXPECT_STREQ(test, m->ctx.client_address, "192.168.0.2");

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "mounthost", "mounter"), 0);
	KUNIT_EXPECT_STREQ(test, m->ctx.mount_server.hostname, "mounter");
}

/* Repeating one of those frees the previous value rather than leaking it. */
static void repeating_clientaddr_replaces_the_previous_value(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "clientaddr", "10.0.0.1"), 0);
	KUNIT_ASSERT_EQ(test, parse_value(test, m, "clientaddr", "10.0.0.2"), 0);
	KUNIT_EXPECT_STREQ(test, m->ctx.client_address, "10.0.0.2");
}

/*
 * fsc has two spellings: a flag that just turns caching on or off, and a
 * value that additionally names the cache. They share a name in the
 * parameter table and fs_lookup_key() picks between them on whether the
 * option arrived with a value.
 */
static void the_fsc_flag_enables_caching(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "fsc"), 0);
	KUNIT_EXPECT_TRUE(test, m->ctx.options & NFS_OPTION_FSCACHE);
	KUNIT_EXPECT_PTR_EQ(test, m->ctx.fscache_uniq, NULL);
}

static void fsc_with_a_value_names_the_cache(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "fsc", "mycache"), 0);
	KUNIT_EXPECT_TRUE(test, m->ctx.options & NFS_OPTION_FSCACHE);
	KUNIT_EXPECT_STREQ(test, m->ctx.fscache_uniq, "mycache");
}

/* nofsc turns caching off and drops any cache name already set. */
static void nofsc_clears_the_cache_name(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_ASSERT_EQ(test, parse_value(test, m, "fsc", "mycache"), 0);
	KUNIT_ASSERT_EQ(test, parse_flag(m, "nofsc"), 0);
	KUNIT_EXPECT_FALSE(test, m->ctx.options & NFS_OPTION_FSCACHE);
	KUNIT_EXPECT_PTR_EQ(test, m->ctx.fscache_uniq, NULL);
}

static void migration_toggles_its_option_bit(struct kunit *test)
{
	struct mount_ctx *m = mount_ctx_new(test);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "migration"), 0);
	KUNIT_EXPECT_TRUE(test, m->ctx.options & NFS_OPTION_MIGRATION);

	KUNIT_EXPECT_EQ(test, parse_flag(m, "nomigration"), 0);
	KUNIT_EXPECT_FALSE(test, m->ctx.options & NFS_OPTION_MIGRATION);
}

/*
 * Options that take one of a fixed set of words. Each one writes a
 * combination of flags rather than a single bit.
 */

struct enum_param {
	const char	*desc;
	const char	*key;
	const char	*value;
	unsigned int	before;
	unsigned int	after;
};

static void enum_get_desc(const struct enum_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct enum_param enum_params[] = {
	{
		.desc	= "lookupcache=all clears both cache bits",
		.key	= "lookupcache", .value = "all",
		.before	= NFS_MOUNT_LOOKUP_CACHE_NONEG |
			  NFS_MOUNT_LOOKUP_CACHE_NONE,
		.after	= 0,
	},
	{
		/* positive caching only: negative entries are not cached */
		.desc	= "lookupcache=pos keeps only NONEG",
		.key	= "lookupcache", .value = "pos",
		.before	= NFS_MOUNT_LOOKUP_CACHE_NONE,
		.after	= NFS_MOUNT_LOOKUP_CACHE_NONEG,
	},
	{
		.desc	= "lookupcache=positive is the same as pos",
		.key	= "lookupcache", .value = "positive",
		.before	= NFS_MOUNT_LOOKUP_CACHE_NONE,
		.after	= NFS_MOUNT_LOOKUP_CACHE_NONEG,
	},
	{
		.desc	= "lookupcache=none sets both cache bits",
		.key	= "lookupcache", .value = "none",
		.before	= 0,
		.after	= NFS_MOUNT_LOOKUP_CACHE_NONEG |
			  NFS_MOUNT_LOOKUP_CACHE_NONE,
	},
	{
		.desc	= "local_lock=all sets both local lock bits",
		.key	= "local_lock", .value = "all",
		.before	= 0,
		.after	= NFS_MOUNT_LOCAL_FLOCK | NFS_MOUNT_LOCAL_FCNTL,
	},
	{
		.desc	= "local_lock=flock sets only FLOCK",
		.key	= "local_lock", .value = "flock",
		.before	= 0,
		.after	= NFS_MOUNT_LOCAL_FLOCK,
	},
	{
		.desc	= "local_lock=posix sets only FCNTL",
		.key	= "local_lock", .value = "posix",
		.before	= 0,
		.after	= NFS_MOUNT_LOCAL_FCNTL,
	},
	{
		.desc	= "local_lock=none clears both local lock bits",
		.key	= "local_lock", .value = "none",
		.before	= NFS_MOUNT_LOCAL_FLOCK | NFS_MOUNT_LOCAL_FCNTL,
		.after	= 0,
	},
	{
		.desc	= "write=lazy clears both write bits",
		.key	= "write", .value = "lazy",
		.before	= NFS_MOUNT_WRITE_EAGER | NFS_MOUNT_WRITE_WAIT,
		.after	= 0,
	},
	{
		.desc	= "write=eager sets EAGER only",
		.key	= "write", .value = "eager",
		.before	= NFS_MOUNT_WRITE_WAIT,
		.after	= NFS_MOUNT_WRITE_EAGER,
	},
	{
		/* wait implies eager: it is eager plus waiting for the write */
		.desc	= "write=wait sets EAGER and WAIT",
		.key	= "write", .value = "wait",
		.before	= 0,
		.after	= NFS_MOUNT_WRITE_EAGER | NFS_MOUNT_WRITE_WAIT,
	},
	{
		.desc	= "fatal_neterrors=none clears NETUNREACH_FATAL",
		.key	= "fatal_neterrors", .value = "none",
		.before	= NFS_MOUNT_NETUNREACH_FATAL,
		.after	= 0,
	},
	{
		.desc	= "fatal_neterrors=ENETDOWN:ENETUNREACH sets it",
		.key	= "fatal_neterrors", .value = "ENETDOWN:ENETUNREACH",
		.before	= 0,
		.after	= NFS_MOUNT_NETUNREACH_FATAL,
	},
	{
		/* the spelling is accepted in either order */
		.desc	= "fatal_neterrors=ENETUNREACH:ENETDOWN sets it",
		.key	= "fatal_neterrors", .value = "ENETUNREACH:ENETDOWN",
		.before	= 0,
		.after	= NFS_MOUNT_NETUNREACH_FATAL,
	},
	{
		/*
		 * In the initial network namespace the default is to keep
		 * retrying, so the bit is cleared.
		 */
		.desc	= "fatal_neterrors=default in init_net clears it",
		.key	= "fatal_neterrors", .value = "default",
		.before	= NFS_MOUNT_NETUNREACH_FATAL,
		.after	= 0,
	},
	{
		.desc	= "rdirplus=none sets NORDIRPLUS",
		.key	= "rdirplus", .value = "none",
		.before	= NFS_MOUNT_FORCE_RDIRPLUS,
		.after	= NFS_MOUNT_NORDIRPLUS,
	},
	{
		.desc	= "rdirplus=force sets FORCE_RDIRPLUS",
		.key	= "rdirplus", .value = "force",
		.before	= NFS_MOUNT_NORDIRPLUS,
		.after	= NFS_MOUNT_FORCE_RDIRPLUS,
	},
};

KUNIT_ARRAY_PARAM(enum, enum_params, enum_get_desc);

static void enum_case(struct kunit *test)
{
	const struct enum_param *param = test->param_value;
	struct mount_ctx *m = mount_ctx_new(test);

	m->ctx.flags = param->before;

	KUNIT_EXPECT_EQ(test,
			parse_value(test, m, param->key, param->value), 0);
	KUNIT_EXPECT_EQ_MSG(test, m->ctx.flags, param->after, "%s=%s",
			    param->key, param->value);
}

static struct kunit_case nfs_mount_flag_cases[] = {
	{
		.name			= "flag options set and clear flags",
		.run_case		= flag_case,
		.generate_params	= flag_gen_params,
	},
	KUNIT_CASE(softreval_has_no_negated_spelling),
	KUNIT_CASE(lock_records_the_requested_state),
	KUNIT_CASE(sloppy_is_recorded),
	KUNIT_CASE(an_unknown_option_is_rejected),
	KUNIT_CASE(sloppy_swallows_an_unknown_option),
	KUNIT_CASE(a_flag_option_rejects_a_value),
	{}
};

static struct kunit_suite nfs_mount_flag_suite = {
	.name		= "nfs-mount-flags",
	.test_cases	= nfs_mount_flag_cases,
};

static struct kunit_case nfs_mount_number_cases[] = {
	KUNIT_CASE(size_options_are_stored),
	KUNIT_CASE(size_options_are_not_bounded_at_parse_time),
	KUNIT_CASE(timeout_options_are_stored),
	KUNIT_CASE(attribute_cache_options_are_stored),
	KUNIT_CASE(actimeo_sets_all_four_timeouts),
	KUNIT_CASE(port_options_are_stored),
	KUNIT_CASE(port_zero_is_accepted),
	KUNIT_CASE(connection_count_options_are_stored),
	KUNIT_CASE(mount_protocol_version_is_stored),
	KUNIT_CASE(minorversion_is_stored),
	{}
};

static struct kunit_suite nfs_mount_number_suite = {
	.name		= "nfs-mount-numbers",
	.test_cases	= nfs_mount_number_cases,
};

static struct kunit_case nfs_mount_reject_cases[] = {
	{
		.name			= "bad values are rejected",
		.run_case		= reject_case,
		.generate_params	= reject_gen_params,
	},
	{}
};

static struct kunit_suite nfs_mount_reject_suite = {
	.name		= "nfs-mount-bad-values",
	.test_cases	= nfs_mount_reject_cases,
};

static struct kunit_case nfs_mount_version_cases[] = {
	{
		.name			= "version strings and flags",
		.run_case		= version_case,
		.generate_params	= version_gen_params,
	},
	KUNIT_CASE(selecting_v4_after_v3_clears_the_v3_bit),
	{}
};

static struct kunit_suite nfs_mount_version_suite = {
	.name		= "nfs-mount-version",
	.test_cases	= nfs_mount_version_cases,
};

static struct kunit_case nfs_mount_sec_cases[] = {
	KUNIT_CASE(sec_maps_each_name_to_its_pseudoflavor),
	KUNIT_CASE(sec_null_is_the_same_as_none),
	KUNIT_CASE(sec_ignores_a_repeated_flavor),
	KUNIT_CASE(sec_skips_empty_list_entries),
	KUNIT_CASE(sec_stops_at_the_first_bad_name),
	{}
};

static struct kunit_suite nfs_mount_sec_suite = {
	.name		= "nfs-mount-sec",
	.test_cases	= nfs_mount_sec_cases,
};

static struct kunit_case nfs_mount_transport_cases[] = {
	KUNIT_CASE(proto_tcp_selects_tcp),
	KUNIT_CASE(proto_tcp6_selects_tcp_over_ipv6),
	KUNIT_CASE(proto_udp_selects_udp_and_clears_the_tcp_flag),
	KUNIT_CASE(proto_udp6_selects_udp_over_ipv6),
	KUNIT_CASE(the_udp_flag_selects_udp),
	KUNIT_CASE(the_tcp_flag_selects_tcp),
	KUNIT_CASE(mountproto_sets_the_side_protocol),
	KUNIT_CASE(mount_transport_follows_the_nfs_transport),
	KUNIT_CASE(an_explicit_mount_transport_is_left_alone),
	KUNIT_CASE(an_unset_transport_defaults_to_tcp),
	KUNIT_CASE(tcp_and_rdma_are_left_as_they_are),
	KUNIT_CASE(udp_is_rejected_for_nfsv4),
	KUNIT_CASE(udp_for_nfsv3_follows_the_build_config),
	KUNIT_CASE(a_tls_policy_promotes_tcp_to_tcp_tls),
	KUNIT_CASE(a_tls_policy_on_rdma_is_rejected),
	KUNIT_CASE(xprtsec_maps_each_policy_name),
	{}
};

static struct kunit_suite nfs_mount_transport_suite = {
	.name		= "nfs-mount-transport",
	.test_cases	= nfs_mount_transport_cases,
};

static struct kunit_case nfs_mount_address_cases[] = {
	KUNIT_CASE(addr_parses_an_ipv4_address),
	KUNIT_CASE(addr_parses_an_ipv6_address),
	KUNIT_CASE(mountaddr_parses_into_the_mount_server),
	{}
};

static struct kunit_suite nfs_mount_address_suite = {
	.name		= "nfs-mount-address",
	.test_cases	= nfs_mount_address_cases,
};

static struct kunit_case nfs_mount_string_cases[] = {
	KUNIT_CASE(source_takes_ownership_of_the_string),
	KUNIT_CASE(a_second_source_is_rejected),
	KUNIT_CASE(clientaddr_and_mounthost_are_kept),
	KUNIT_CASE(repeating_clientaddr_replaces_the_previous_value),
	KUNIT_CASE(the_fsc_flag_enables_caching),
	KUNIT_CASE(fsc_with_a_value_names_the_cache),
	KUNIT_CASE(nofsc_clears_the_cache_name),
	KUNIT_CASE(migration_toggles_its_option_bit),
	{}
};

static struct kunit_suite nfs_mount_string_suite = {
	.name		= "nfs-mount-strings",
	.test_cases	= nfs_mount_string_cases,
};

static struct kunit_case nfs_mount_enum_cases[] = {
	{
		.name			= "word-valued options set flags",
		.run_case		= enum_case,
		.generate_params	= enum_gen_params,
	},
	{}
};

static struct kunit_suite nfs_mount_enum_suite = {
	.name		= "nfs-mount-enums",
	.test_cases	= nfs_mount_enum_cases,
};

kunit_test_suites(&nfs_mount_flag_suite,
		  &nfs_mount_number_suite,
		  &nfs_mount_reject_suite,
		  &nfs_mount_version_suite,
		  &nfs_mount_sec_suite,
		  &nfs_mount_transport_suite,
		  &nfs_mount_address_suite,
		  &nfs_mount_string_suite,
		  &nfs_mount_enum_suite);

MODULE_DESCRIPTION("Test NFS mount option parsing");
MODULE_LICENSE("GPL");
