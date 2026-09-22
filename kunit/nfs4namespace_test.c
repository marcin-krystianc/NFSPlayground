// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for fs/nfs/nfs4namespace.c: NFSv4 pathname handling and the
 * fs_locations-driven decisions that build on it.
 *
 * This file has two layers. Underneath are pure string/array functions --
 * turning an XDR-decoded nfs4_pathname into a posix path, splitting the
 * "path" back out of a "server:path" string, and building the path a
 * dentry corresponds to. On top of that sit the referral and migration
 * decisions (nfs_follow_referral, nfs4_submount, nfs4_negotiate_security,
 * nfs4_replace_transport) that consume fs_locations replies and act on
 * them.
 *
 * The top layer is where this file earns its keep, but almost all of it
 * is reachable only by finishing the RPCs it starts: nfs_find_best_sec()
 * and nfs4_negotiate_security() clone a live rpc_clnt onto a real
 * transport (rpc_clone_client_set_auth() dereferences clnt->cl_xprt's
 * RCU-protected transport and switch -- there is no seam short of a real
 * xprt), and try_location()/nfs_follow_referral()/nfs_do_refmount()/
 * nfs4_submount() drive actual LOOKUP/GETFH/FS_LOCATIONS RPCs against a
 * server. None of that is reachable from a unit test the way the xfstests
 * ports reach it over the real loopback mount; testing it here would mean
 * faking enough of net/sunrpc to no longer be testing this file. docs/
 * kunit-nfs.md's "few dozen pure decision functions underneath [RPC
 * calls]" framing for nfs4proc.c applies here too.
 *
 * What is reachable, and covered below:
 *   - nfs4_pathname_len / nfs4_pathname_string: XDR pathname -> posix path,
 *     including both length limits (NAME_MAX per component, PATH_MAX
 *     overall).
 *   - nfs_path_component: splitting "path" out of "server:path", including
 *     the bracketed-IPv6-literal form.
 *   - nfs4_path: the dentry-to-path wrapper, driven with a self-parented
 *     (root) dentry -- IS_ROOT() short-circuits nfs_path() before it walks
 *     d_parent, which is the only part of nfs_path() reachable without a
 *     real mounted tree.
 *   - nfs4_validate_fspath: the fs_root-is-a-prefix check on top of the
 *     above two.
 *   - nfs_parse_server_name: the address-literal and universal-address
 *     ("a.b.c.d.p1.p2") parse paths. The DNS-resolution fallback
 *     (nfs_dns_resolve_name(), reached only when neither parse succeeds)
 *     is not exercised: it issues a real request_key() upcall, which this
 *     environment has no /sbin/request-key for, so every test input here
 *     is crafted to succeed or fail before that fallback is reached.
 *   - nfs4_replace_transport: the location/server list walk -- null and
 *     empty locations, per-location validity (nservers, rootpath), and
 *     per-server validity (empty/oversized name, an IPv6 zone id, an
 *     unparseable name) -- up to but not including a successful match,
 *     which would call nfs4_update_server() and needs a live nfs_client
 *     and transport the same way the security/referral functions above
 *     do. Reachable indirectly (it is not un-staticed, just called
 *     through nfs4_replace_transport()) rather than tested directly for
 *     the same reason.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/dcache.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/nfs.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/rcupdate.h>
#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/xprt.h>
#include <linux/version.h>
#include <net/net_namespace.h>

#include "internal.h"
#include "nfs4_fs.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Private to nfs4namespace.c; un-staticed by scripts/kunit/run-nfs-kunit.sh. */
ssize_t nfs4_pathname_len(const struct nfs4_pathname *pathname);
char *nfs4_pathname_string(const struct nfs4_pathname *pathname,
			   unsigned short *_len);
char *nfs4_path(struct dentry *dentry, char *buffer, ssize_t buflen);
int nfs4_validate_fspath(struct dentry *dentry,
			 const struct nfs4_fs_locations *locations,
			 struct nfs_fs_context *ctx);

/*
 * nfs_path_component() is declared "static char *" on one line rather
 * than split across two like its neighbours, so it needs its own entry
 * rather than sharing nfs4_path's; noted here because the two look like
 * they should be interchangeable at a glance.
 */
char *nfs_path_component(const char *nfspath, const char *end);

/*
 * nfs4_pathname_len() / nfs4_pathname_string(): turning the XDR-decoded
 * component array into a posix path string, with the NAME_MAX-per-
 * component and PATH_MAX-overall limits the protocol has no bound on
 * otherwise.
 */

static void set_component(struct nfs4_string *c, char *data, unsigned int len)
{
	c->data = data;
	c->len = len;
}

/*
 * struct nfs4_pathname embeds components[NFS4_PATHNAME_MAXCOMPONENTS]
 * (512 entries, ~8KB) directly rather than behind a pointer, so -- same
 * reasoning as struct nfs4_fs_locations near the bottom of this file --
 * every instance here is heap-allocated rather than a local variable.
 */
static struct nfs4_pathname *pathname_new(struct kunit *test)
{
	struct nfs4_pathname *path = kunit_kzalloc(test, sizeof(*path),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	return path;
}

static void pathname_len_of_zero_components_is_zero(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);

	KUNIT_EXPECT_EQ(test, nfs4_pathname_len(path), 0);
}

/* Each component costs its length plus one, for the leading '/'. */
static void pathname_len_sums_components_plus_separators(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);

	path->ncomponents = 2;
	set_component(&path->components[0], "aa", 2);
	set_component(&path->components[1], "bbb", 3);

	/* "/aa" (3) + "/bbb" (4) */
	KUNIT_EXPECT_EQ(test, nfs4_pathname_len(path), 7);
}

static void a_component_over_name_max_is_too_long(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	char *component = kunit_kzalloc(test, NAME_MAX + 2, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, component);
	memset(component, 'a', NAME_MAX + 1);
	path->ncomponents = 1;
	set_component(&path->components[0], component, NAME_MAX + 1);

	KUNIT_EXPECT_EQ(test, nfs4_pathname_len(path), -ENAMETOOLONG);
}

/* A component of exactly NAME_MAX is not "over" it. */
static void a_component_of_exactly_name_max_is_not_too_long(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	char *component = kunit_kzalloc(test, NAME_MAX, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, component);
	memset(component, 'a', NAME_MAX);
	path->ncomponents = 1;
	set_component(&path->components[0], component, NAME_MAX);

	KUNIT_EXPECT_EQ(test, nfs4_pathname_len(path), NAME_MAX + 1);
}

/* Individually short components can still sum past PATH_MAX. */
static void components_summing_past_path_max_are_too_long(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	char *component = kunit_kzalloc(test, 64, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, component);
	memset(component, 'a', 64);

	/* (64 + 1) * 64 = 4160 > PATH_MAX (4096) */
	for (i = 0; i < 64; i++)
		set_component(&path->components[path->ncomponents++],
			     component, 64);

	KUNIT_EXPECT_EQ(test, nfs4_pathname_len(path), -ENAMETOOLONG);
}

static void pathname_string_builds_a_posix_path(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	unsigned short len = 0;
	char *result;

	path->ncomponents = 3;
	set_component(&path->components[0], "export", 6);
	set_component(&path->components[1], "a", 1);
	set_component(&path->components[2], "b", 1);

	result = nfs4_pathname_string(path, &len);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, result);
	KUNIT_EXPECT_STREQ(test, result, "/export/a/b");
	KUNIT_EXPECT_EQ(test, (unsigned int)len, strlen("/export/a/b"));
	kfree(result);
}

static void pathname_string_of_zero_components_is_empty(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	unsigned short len = 0xffff;
	char *result;

	result = nfs4_pathname_string(path, &len);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, result);
	KUNIT_EXPECT_STREQ(test, result, "");
	KUNIT_EXPECT_EQ(test, len, 0);
	kfree(result);
}

/* An over-length pathname is rejected before any buffer is allocated. */
static void pathname_string_propagates_the_length_error(struct kunit *test)
{
	struct nfs4_pathname *path = pathname_new(test);
	char *component = kunit_kzalloc(test, NAME_MAX + 2, GFP_KERNEL);
	unsigned short len;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, component);
	memset(component, 'a', NAME_MAX + 1);
	path->ncomponents = 1;
	set_component(&path->components[0], component, NAME_MAX + 1);

	KUNIT_EXPECT_EQ(test, PTR_ERR(nfs4_pathname_string(path, &len)),
			-ENAMETOOLONG);
}

/*
 * nfs_path_component(): splitting the path out of a "<server>:<path>"
 * device-name string, including the bracketed-IPv6-literal spelling.
 */

static void path_component_splits_on_the_first_colon(struct kunit *test)
{
	static const char s[] = "server:/export/a";

	KUNIT_EXPECT_STREQ(test,
			   nfs_path_component(s, s + strlen(s)),
			   "/export/a");
}

/* A second colon in the path itself does not confuse the split. */
static void path_component_splits_only_on_the_first_colon(struct kunit *test)
{
	static const char s[] = "server:/export:weird";

	KUNIT_EXPECT_STREQ(test,
			   nfs_path_component(s, s + strlen(s)),
			   "/export:weird");
}

static void path_component_with_no_colon_returns_null(struct kunit *test)
{
	static const char s[] = "not-a-device-string";

	KUNIT_EXPECT_PTR_EQ(test, nfs_path_component(s, s + strlen(s)), NULL);
}

/* A colon at or beyond "end" does not count as the split point. */
static void path_component_ignores_a_colon_past_end(struct kunit *test)
{
	static const char s[] = "server:/export";
	/* end == the colon itself, so nothing at or after it is in range */
	const char *end = s + 6;

	KUNIT_EXPECT_PTR_EQ(test, nfs_path_component(s, end), NULL);
}

static void path_component_parses_a_bracketed_ipv6_literal(struct kunit *test)
{
	static const char s[] = "[::1]:/export";

	KUNIT_EXPECT_STREQ(test,
			   nfs_path_component(s, s + strlen(s)),
			   "/export");
}

/* A bracketed literal with no closing ']' is not a valid split. */
static void path_component_rejects_an_unterminated_bracket(struct kunit *test)
{
	static const char s[] = "[::1:/export";

	KUNIT_EXPECT_PTR_EQ(test, nfs_path_component(s, s + strlen(s)), NULL);
}

/* The closing bracket must be immediately followed by ':', not by the path. */
static void path_component_rejects_a_bracket_not_followed_by_colon(struct kunit *test)
{
	static const char s[] = "[::1]/export";

	KUNIT_EXPECT_PTR_EQ(test, nfs_path_component(s, s + strlen(s)), NULL);
}

/*
 * nfs4_path(): the dentry-to-path wrapper around nfs_path(). Driven with
 * a self-parented dentry, which IS_ROOT() (dentry == dentry->d_parent)
 * recognises without walking d_parent -- the only part of nfs_path()
 * reachable without a real mounted dentry tree. NFS stores each export's
 * root path string in d_fsdata (see nfs_path()); zero-initializing the
 * dentry leaves d_lockref's embedded spinlock unlocked, which is all
 * nfs_path() needs from it, and this kunit config runs without
 * CONFIG_DEBUG_SPINLOCK/LOCKDEP to object to the lock never having seen
 * spin_lock_init().
 */

static struct dentry *root_dentry_new(struct kunit *test, char *fsdata)
{
	struct dentry *dentry = kunit_kzalloc(test, sizeof(*dentry),
					      GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dentry);
	dentry->d_parent = dentry;
	dentry->d_fsdata = fsdata;
	return dentry;
}

/*
 * A root dentry has no component of its own to prepend, so nfs_path()'s
 * canonicalisation step (NFS_PATH_CANONICAL) always inserts a separator
 * between that empty prefix and d_fsdata, before d_fsdata is even read.
 * Pinned here because it means a bare root's path is "<fsdata>/", with a
 * trailing slash, regardless of whether d_fsdata itself starts with one.
 *
 * nfs_path_component() finds no colon in this plain filesystem path, so
 * nfs4_path() falls back to the raw path nfs_path() built -- this is the
 * ordinary case, "server:path" strings only occur in fc->source, never
 * in a dentry's own path.
 */
static void path_of_a_root_dentry_gets_a_trailing_slash(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export");
	char buf[64];
	char *path;

	path = nfs4_path(dentry, buf, sizeof(buf));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_EXPECT_STREQ(test, path, "/export/");
}

/* The same trailing slash is added whether or not d_fsdata has a leading one. */
static void path_of_a_root_dentry_without_a_leading_slash_also_gets_one(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "export");
	char buf[64];
	char *path;

	path = nfs4_path(dentry, buf, sizeof(buf));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_EXPECT_STREQ(test, path, "export/");
}

/*
 * d_fsdata already ending in one or more slashes does not produce a
 * doubled-up trailing slash: nfs_path() strips them from d_fsdata before
 * appending its own single canonical one, so the result is idempotent.
 */
static void path_of_a_root_dentry_strips_existing_trailing_slashes(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export///");
	char buf[64];
	char *path;

	path = nfs4_path(dentry, buf, sizeof(buf));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_EXPECT_STREQ(test, path, "/export/");
}

/* A buffer too small for d_fsdata's contents fails with -ENAMETOOLONG. */
static void path_of_a_root_dentry_with_too_small_a_buffer_fails(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export");
	char buf[4];
	char *path;

	path = nfs4_path(dentry, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, PTR_ERR(path), -ENAMETOOLONG);
}

/*
 * nfs4_validate_fspath(): fs_locations::fs_root has to be a prefix of the
 * path the client thinks it is looking up under, or the referral is
 * rejected rather than followed somewhere unexpected.
 */

static struct nfs4_fs_locations *locations_new(struct kunit *test)
{
	struct nfs4_fs_locations *locations = kunit_kzalloc(test,
							     sizeof(*locations),
							     GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, locations);
	return locations;
}

static void validate_fspath_accepts_a_matching_root(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export");
	struct nfs4_fs_locations *locations = locations_new(test);
	struct nfs_fs_context *ctx = kunit_kzalloc(test, sizeof(*ctx),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	/*
	 * dentry's own path is "/export/" (nfs4_path always appends a
	 * trailing slash for a root dentry -- see the nfs4-dentry-path
	 * suite). nfs4_pathname_string() always prepends a leading slash of
	 * its own, so a single "export" component becomes "/export", which
	 * is a prefix of "/export/".
	 */
	locations->fs_path.ncomponents = 1;
	set_component(&locations->fs_path.components[0], "export", 6);

	KUNIT_EXPECT_EQ(test, nfs4_validate_fspath(dentry, locations, ctx), 0);
}

static void validate_fspath_rejects_a_mismatched_root(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export");
	struct nfs4_fs_locations *locations = locations_new(test);
	struct nfs_fs_context *ctx = kunit_kzalloc(test, sizeof(*ctx),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	locations->fs_path.ncomponents = 1;
	set_component(&locations->fs_path.components[0], "elsewhere", 9);

	KUNIT_EXPECT_EQ(test, nfs4_validate_fspath(dentry, locations, ctx),
			-ENOENT);
}

/* A malformed locations->fs_path (over NAME_MAX) fails, not crashes. */
static void validate_fspath_propagates_a_malformed_fs_path(struct kunit *test)
{
	struct dentry *dentry = root_dentry_new(test, "/export");
	struct nfs4_fs_locations *locations = locations_new(test);
	struct nfs_fs_context *ctx = kunit_kzalloc(test, sizeof(*ctx),
						   GFP_KERNEL);
	char *component = kunit_kzalloc(test, NAME_MAX + 2, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, component);
	memset(component, 'a', NAME_MAX + 1);
	locations->fs_path.ncomponents = 1;
	set_component(&locations->fs_path.components[0], component,
		     NAME_MAX + 1);

	KUNIT_EXPECT_EQ(test, nfs4_validate_fspath(dentry, locations, ctx),
			-ENAMETOOLONG);
}

/*
 * nfs_parse_server_name(): a location server name is either an address
 * literal (rpc_pton) or a universal address "a.b.c.d.p1.p2" (32-bit port
 * split into two base-256 digits, rpc_uaddr2sockaddr) -- both pure string
 * parsing. A name that is neither falls through to DNS resolution, not
 * exercised here (see the file comment).
 */

static void parse_server_name_accepts_an_ipv4_literal(struct kunit *test)
{
	struct sockaddr_storage ss = {};
	static const char s[] = "192.0.2.1";
	size_t salen;

	salen = nfs_parse_server_name((char *)s, strlen(s), &ss,
				      sizeof(ss), &init_net, 0);
	KUNIT_ASSERT_EQ(test, salen, sizeof(struct sockaddr_in));
	KUNIT_EXPECT_EQ(test,
			((struct sockaddr_in *)&ss)->sin_addr.s_addr,
			htonl(0xC0000201));
}

static void parse_server_name_accepts_an_ipv6_literal(struct kunit *test)
{
	struct sockaddr_storage ss = {};
	static const char s[] = "2001:db8::1";
	size_t salen;

	salen = nfs_parse_server_name((char *)s, strlen(s), &ss,
				      sizeof(ss), &init_net, 0);
	KUNIT_EXPECT_EQ(test, salen, sizeof(struct sockaddr_in6));
}

/* A caller-supplied port is only applied when the literal itself parsed. */
static void parse_server_name_applies_an_explicit_port(struct kunit *test)
{
	struct sockaddr_storage ss = {};
	static const char s[] = "192.0.2.1";
	size_t salen;

	salen = nfs_parse_server_name((char *)s, strlen(s), &ss,
				      sizeof(ss), &init_net, 2049);
	KUNIT_ASSERT_EQ(test, salen, sizeof(struct sockaddr_in));
	KUNIT_EXPECT_EQ(test,
			((struct sockaddr_in *)&ss)->sin_port, htons(2049));
}

/* "a.b.c.d.p1.p2": a universal address, not a (too-long) IPv4 literal. */
static void parse_server_name_accepts_a_universal_address(struct kunit *test)
{
	struct sockaddr_storage ss = {};
	/* 192.0.2.1, port 2049 = 0x0801 -> p1=8, p2=1 */
	static const char s[] = "192.0.2.1.8.1";
	size_t salen;

	salen = nfs_parse_server_name((char *)s, strlen(s), &ss,
				      sizeof(ss), &init_net, 0);
	KUNIT_ASSERT_EQ(test, salen, sizeof(struct sockaddr_in));
	KUNIT_EXPECT_EQ(test,
			((struct sockaddr_in *)&ss)->sin_addr.s_addr,
			htonl(0xC0000201));
	KUNIT_EXPECT_EQ(test,
			((struct sockaddr_in *)&ss)->sin_port, htons(2049));
}

/*
 * nfs4_replace_transport(): walking the fs_locations array to find a
 * server worth switching to. Every server string below is crafted to be
 * rejected before nfs_parse_server_name() would be reached, so the walk
 * never gets far enough to call nfs4_update_server() -- see the file
 * comment for why that boundary is where this stops.
 *
 * nfs4_try_replacing_one_location() reads server->client unconditionally,
 * before any of the per-server checks, to find the net namespace to parse
 * addresses in (rpc_net_ns(), a plain clnt->cl_xprt->xprt_net read). Any
 * case where a location passes nfs4_replace_transport()'s own two checks
 * (nservers, rootpath) needs a real client for that dereference, even
 * though nothing else about the client is ever touched -- confirmed the
 * hard way: a zeroed nfs_server here reaches that dereference on NULL and
 * the whole run hangs, kunit.py's subprocess timeout the only thing that
 * ever reports it.
 */

static struct nfs_server *server_with_a_client(struct kunit *test)
{
	struct rpc_xprt *xprt = kunit_kzalloc(test, sizeof(*xprt), GFP_KERNEL);
	struct rpc_clnt *clnt = kunit_kzalloc(test, sizeof(*clnt), GFP_KERNEL);
	struct nfs_server *server = kunit_kzalloc(test, sizeof(*server),
						  GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, xprt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clnt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, server);
	xprt->xprt_net = &init_net;
	RCU_INIT_POINTER(clnt->cl_xprt, xprt);
	server->client = clnt;
	return server;
}

static void replace_transport_with_no_locations_fails(struct kunit *test)
{
	struct nfs_server server = {};

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(&server, NULL), -ENOENT);
}

/*
 * struct nfs4_fs_locations is far too big for the kernel stack --
 * NFS4_FS_LOCATIONS_MAXENTRIES (10) locations, each with
 * NFS4_FS_LOCATION_MAXSERVERS (10) servers and an NFS4_PATHNAME_MAXCOMPONENTS
 * (512) component rootpath, comes to roughly 80KB -- so every case here
 * uses locations_new()'s kunit_kzalloc() rather than a local variable.
 * Confirmed the hard way: a stack-local one here overflowed the kthread
 * stack, corrupting enough state that UML's own SIGSEGV handler could not
 * recover, which looked exactly like an indefinite hang.
 */
static void replace_transport_with_zero_locations_fails(struct kunit *test)
{
	struct nfs_server server = {};
	struct nfs4_fs_locations *locations = locations_new(test);

	locations->nlocations = 0;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(&server, locations),
			-ENOENT);
}

/*
 * Before v6.19 (confirmed present on v6.12.57 and v6.18.52, gone by
 * v7.2.6, part of the CI matrix in .github/workflows/kunit.yml),
 * nfs4_replace_transport() pre-allocated two pages for
 * nfs4_try_replacing_one_location() to use, ahead of the per-location
 * loop, setting "error = -ENOMEM" first and only overwriting it once a
 * location actually reaches that call. When every location in the array
 * is skipped by the per-location guard -- as in both cases below -- that
 * leftover -ENOMEM came back on those kernels instead of the -ENOENT
 * this file otherwise asserts throughout. That is that old code's own
 * behaviour, not something worth asserting either way here, so both
 * cases skip on a kernel old enough to have it rather than loosen the
 * assertion to match it.
 *
 * LINUX_VERSION_CODE, not a feature probe, because there is nothing this
 * old code path leaves behind to detect -- no dropped symbol, no changed
 * struct, just a returned value. 7.0.0 as the cutoff is arbitrary within
 * the actual boundary (confirmed only to lie somewhere between v6.18.52
 * and v7.2.6): it is not meant to be precise, only to sort every ref in
 * the CI matrix onto the correct side of it.
 */
static bool kernel_predates_the_replace_transport_page_removal(void)
{
	return LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0);
}

/*
 * A location with no servers listed is skipped without being tried, so
 * this (like the empty-rootpath case below) never reaches
 * nfs4_try_replacing_one_location() and needs no client fixture.
 */
static void replace_transport_skips_a_location_with_no_servers(struct kunit *test)
{
	struct nfs_server server = {};
	struct nfs4_fs_locations *locations = locations_new(test);

	if (kernel_predates_the_replace_transport_page_removal())
		kunit_skip(test, "pre-v7 nfs4_replace_transport() returns a stale -ENOMEM here, see comment");

	locations->nlocations = 1;
	locations->locations[0].nservers = 0;
	locations->locations[0].rootpath.ncomponents = 1;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(&server, locations),
			-ENOENT);
}

/* A location with an empty rootpath is skipped the same way. */
static void replace_transport_skips_a_location_with_no_rootpath(struct kunit *test)
{
	struct nfs_server server = {};
	struct nfs4_fs_locations *locations = locations_new(test);

	if (kernel_predates_the_replace_transport_page_removal())
		kunit_skip(test, "pre-v7 nfs4_replace_transport() returns a stale -ENOMEM here, see comment");

	locations->nlocations = 1;
	locations->locations[0].nservers = 1;
	set_component(&locations->locations[0].servers[0], "192.0.2.1", 9);
	locations->locations[0].rootpath.ncomponents = 0;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(&server, locations),
			-ENOENT);
}

/*
 * A location that passes both outer checks is tried; every server name
 * in it is invalid in a way nfs4_try_replacing_one_location() catches
 * before calling nfs_parse_server_name(), so the attempt still ends in
 * -ENOENT rather than reaching the network. Each of these does reach
 * nfs4_try_replacing_one_location(), so each needs server_with_a_client().
 */
static void replace_transport_skips_a_zero_length_server_name(struct kunit *test)
{
	struct nfs_server *server = server_with_a_client(test);
	struct nfs4_fs_locations *locations = locations_new(test);

	locations->nlocations = 1;
	locations->locations[0].nservers = 1;
	set_component(&locations->locations[0].servers[0], "", 0);
	locations->locations[0].rootpath.ncomponents = 1;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(server, locations),
			-ENOENT);
}

static void replace_transport_skips_an_oversized_server_name(struct kunit *test)
{
	struct nfs_server *server = server_with_a_client(test);
	struct nfs4_fs_locations *locations = locations_new(test);
	char *oversized = kunit_kzalloc(test, PAGE_SIZE + 2, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, oversized);
	memset(oversized, '1', PAGE_SIZE + 1);
	locations->nlocations = 1;
	locations->locations[0].nservers = 1;
	set_component(&locations->locations[0].servers[0], oversized,
		     PAGE_SIZE + 1);
	locations->locations[0].rootpath.ncomponents = 1;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(server, locations),
			-ENOENT);
}

/* A server name carrying an IPv6 zone id ('%') is skipped, not parsed. */
static void replace_transport_skips_a_scoped_ipv6_server_name(struct kunit *test)
{
	struct nfs_server *server = server_with_a_client(test);
	struct nfs4_fs_locations *locations = locations_new(test);

	locations->nlocations = 1;
	locations->locations[0].nservers = 1;
	set_component(&locations->locations[0].servers[0], "fe80::1%eth0", 12);
	locations->locations[0].rootpath.ncomponents = 1;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(server, locations),
			-ENOENT);
}

/*
 * Every location in the array is tried in order, not just the first one
 * that passes the outer checks. The first of the three never reaches
 * nfs4_try_replacing_one_location(), but the other two do, so this still
 * needs the client fixture.
 */
static void replace_transport_tries_every_location(struct kunit *test)
{
	struct nfs_server *server = server_with_a_client(test);
	struct nfs4_fs_locations *locations = locations_new(test);

	locations->nlocations = 3;
	locations->locations[0].nservers = 0; /* skipped outright */
	locations->locations[1].nservers = 1;
	set_component(&locations->locations[1].servers[0], "", 0); /* skipped */
	locations->locations[1].rootpath.ncomponents = 1;
	locations->locations[2].nservers = 1;
	set_component(&locations->locations[2].servers[0], "fe80::1%eth0", 12);
	locations->locations[2].rootpath.ncomponents = 1;

	KUNIT_EXPECT_EQ(test, nfs4_replace_transport(server, locations),
			-ENOENT);
}

static struct kunit_case nfs4_pathname_cases[] = {
	KUNIT_CASE(pathname_len_of_zero_components_is_zero),
	KUNIT_CASE(pathname_len_sums_components_plus_separators),
	KUNIT_CASE(a_component_over_name_max_is_too_long),
	KUNIT_CASE(a_component_of_exactly_name_max_is_not_too_long),
	KUNIT_CASE(components_summing_past_path_max_are_too_long),
	KUNIT_CASE(pathname_string_builds_a_posix_path),
	KUNIT_CASE(pathname_string_of_zero_components_is_empty),
	KUNIT_CASE(pathname_string_propagates_the_length_error),
	{}
};

static struct kunit_suite nfs4_pathname_suite = {
	.name		= "nfs4-pathname",
	.test_cases	= nfs4_pathname_cases,
};

static struct kunit_case nfs4_path_component_cases[] = {
	KUNIT_CASE(path_component_splits_on_the_first_colon),
	KUNIT_CASE(path_component_splits_only_on_the_first_colon),
	KUNIT_CASE(path_component_with_no_colon_returns_null),
	KUNIT_CASE(path_component_ignores_a_colon_past_end),
	KUNIT_CASE(path_component_parses_a_bracketed_ipv6_literal),
	KUNIT_CASE(path_component_rejects_an_unterminated_bracket),
	KUNIT_CASE(path_component_rejects_a_bracket_not_followed_by_colon),
	{}
};

static struct kunit_suite nfs4_path_component_suite = {
	.name		= "nfs4-device-string-path",
	.test_cases	= nfs4_path_component_cases,
};

static struct kunit_case nfs4_path_cases[] = {
	KUNIT_CASE(path_of_a_root_dentry_gets_a_trailing_slash),
	KUNIT_CASE(path_of_a_root_dentry_without_a_leading_slash_also_gets_one),
	KUNIT_CASE(path_of_a_root_dentry_strips_existing_trailing_slashes),
	KUNIT_CASE(path_of_a_root_dentry_with_too_small_a_buffer_fails),
	{}
};

static struct kunit_suite nfs4_path_suite = {
	.name		= "nfs4-dentry-path",
	.test_cases	= nfs4_path_cases,
};

static struct kunit_case nfs4_validate_fspath_cases[] = {
	KUNIT_CASE(validate_fspath_accepts_a_matching_root),
	KUNIT_CASE(validate_fspath_rejects_a_mismatched_root),
	KUNIT_CASE(validate_fspath_propagates_a_malformed_fs_path),
	{}
};

static struct kunit_suite nfs4_validate_fspath_suite = {
	.name		= "nfs4-validate-fspath",
	.test_cases	= nfs4_validate_fspath_cases,
};

static struct kunit_case nfs4_parse_server_name_cases[] = {
	KUNIT_CASE(parse_server_name_accepts_an_ipv4_literal),
	KUNIT_CASE(parse_server_name_accepts_an_ipv6_literal),
	KUNIT_CASE(parse_server_name_applies_an_explicit_port),
	KUNIT_CASE(parse_server_name_accepts_a_universal_address),
	{}
};

static struct kunit_suite nfs4_parse_server_name_suite = {
	.name		= "nfs4-parse-server-name",
	.test_cases	= nfs4_parse_server_name_cases,
};

static struct kunit_case nfs4_replace_transport_cases[] = {
	KUNIT_CASE(replace_transport_with_no_locations_fails),
	KUNIT_CASE(replace_transport_with_zero_locations_fails),
	KUNIT_CASE(replace_transport_skips_a_location_with_no_servers),
	KUNIT_CASE(replace_transport_skips_a_location_with_no_rootpath),
	KUNIT_CASE(replace_transport_skips_a_zero_length_server_name),
	KUNIT_CASE(replace_transport_skips_an_oversized_server_name),
	KUNIT_CASE(replace_transport_skips_a_scoped_ipv6_server_name),
	KUNIT_CASE(replace_transport_tries_every_location),
	{}
};

static struct kunit_suite nfs4_replace_transport_suite = {
	.name		= "nfs4-replace-transport",
	.test_cases	= nfs4_replace_transport_cases,
};

kunit_test_suites(&nfs4_pathname_suite,
		  &nfs4_path_component_suite,
		  &nfs4_path_suite,
		  &nfs4_validate_fspath_suite,
		  &nfs4_parse_server_name_suite,
		  &nfs4_replace_transport_suite);

MODULE_DESCRIPTION("Test NFSv4 namespace pathname and fs_locations handling");
MODULE_LICENSE("GPL");
