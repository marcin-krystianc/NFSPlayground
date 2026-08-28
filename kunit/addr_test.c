// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for SunRPC presentation- and universal-address conversion,
 * implemented in net/sunrpc/addr.c.
 *
 * addr.c is pure logic: string to sockaddr and back, with no I/O and no
 * locking, which makes it one of the few parts of SunRPC that unit tests
 * suit. rpc_pton() in particular is the primitive every NFS mount option
 * that carries an address is parsed through.
 */

#include <kunit/test.h>

#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/inet.h>
#include <linux/string.h>
#include <net/net_namespace.h>

#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/msg_prot.h>

#define ADDRBUF_LEN	(RPCBIND_MAXUADDRLEN + 1)

static size_t expected_salen(unsigned short family)
{
	return family == AF_INET ? sizeof(struct sockaddr_in)
				 : sizeof(struct sockaddr_in6);
}

/*
 * Presentation strings rpc_ntop() is expected to emit verbatim, so one
 * table can drive parsing, formatting and round-trip cases alike. The
 * IPv6 entries deliberately cover the shorthands addr.c special-cases:
 * the unspecified and loopback addresses, and the v4-mapped form.
 */
struct addr_param {
	const char	*desc;
	const char	*presentation;
	unsigned short	family;
};

static void addr_get_desc(const struct addr_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct addr_param canonical_params[] = {
	{ "ipv4 unspecified",	"0.0.0.0",		AF_INET },
	{ "ipv4 loopback",	"127.0.0.1",		AF_INET },
	{ "ipv4 private",	"192.168.1.1",		AF_INET },
	{ "ipv4 broadcast",	"255.255.255.255",	AF_INET },
#if IS_ENABLED(CONFIG_IPV6)
	{ "ipv6 unspecified",	"::",			AF_INET6 },
	{ "ipv6 loopback",	"::1",			AF_INET6 },
	{ "ipv6 compressed",	"2001:db8::1",		AF_INET6 },
	{ "ipv6 v4mapped",	"::ffff:192.168.1.1",	AF_INET6 },
#endif
};

KUNIT_ARRAY_PARAM(canonical_addr, canonical_params, addr_get_desc);

struct invalid_param {
	const char	*desc;
	const char	*input;
};

static void invalid_get_desc(const struct invalid_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

/*
 * Test cases
 */

static void pton_canonical_case(struct kunit *test)
{
	const struct addr_param *param = test->param_value;
	struct sockaddr_storage ss;
	size_t len;

	memset(&ss, 0, sizeof(ss));
	len = rpc_pton(&init_net, param->presentation,
		       strlen(param->presentation),
		       (struct sockaddr *)&ss, sizeof(ss));

	KUNIT_ASSERT_EQ_MSG(test, len, expected_salen(param->family),
			    "rpc_pton() rejected \"%s\"",
			    param->presentation);
	KUNIT_EXPECT_EQ(test, ss.ss_family, param->family);
}

static void ntop_canonical_case(struct kunit *test)
{
	const struct addr_param *param = test->param_value;
	struct sockaddr_storage ss;
	char buf[ADDRBUF_LEN];
	size_t len;

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, param->presentation,
				 strlen(param->presentation),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);

	memset(buf, 0, sizeof(buf));
	len = rpc_ntop((struct sockaddr *)&ss, buf, sizeof(buf));

	KUNIT_EXPECT_EQ(test, len, strlen(param->presentation));
	KUNIT_EXPECT_STREQ(test, buf, param->presentation);
}

/* sockaddr -> string -> sockaddr must preserve the address exactly. */
static void roundtrip_case(struct kunit *test)
{
	const struct addr_param *param = test->param_value;
	struct sockaddr_storage first, second;
	char buf[ADDRBUF_LEN];

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	memset(buf, 0, sizeof(buf));

	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, param->presentation,
				 strlen(param->presentation),
				 (struct sockaddr *)&first, sizeof(first)), 0);
	KUNIT_ASSERT_NE(test,
			rpc_ntop((struct sockaddr *)&first, buf, sizeof(buf)), 0);
	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, buf, strlen(buf),
				 (struct sockaddr *)&second, sizeof(second)), 0);

	KUNIT_EXPECT_EQ(test, first.ss_family, second.ss_family);
	KUNIT_EXPECT_TRUE_MSG(test,
			      rpc_cmp_addr((struct sockaddr *)&first,
					   (struct sockaddr *)&second),
			      "round trip through \"%s\" changed the address",
			      buf);
}

static const struct invalid_param invalid_presentations[] = {
	{ "empty string",		"" },
	{ "not an address",		"notanaddress" },
	{ "ipv4 octet overflow",	"999.1.1.1" },
	{ "ipv4 too few octets",	"192.168.1" },
	{ "ipv4 too many octets",	"192.168.1.1.1" },
	{ "ipv4 trailing dot",		"192.168.1.1." },
	{ "ipv4 non-digit octet",	"192.168.1.x" },
};

KUNIT_ARRAY_PARAM(invalid_presentation, invalid_presentations,
		  invalid_get_desc);

static void pton_invalid_case(struct kunit *test)
{
	const struct invalid_param *param = test->param_value;
	struct sockaddr_storage ss;

	memset(&ss, 0, sizeof(ss));
	KUNIT_EXPECT_EQ_MSG(test,
			    rpc_pton(&init_net, param->input,
				     strlen(param->input),
				     (struct sockaddr *)&ss, sizeof(ss)), 0,
			    "rpc_pton() accepted \"%s\"", param->input);
}

/* addr.c:148 rejects a destination too small to hold the result. */
static void pton_salen_too_small(struct kunit *test)
{
	struct sockaddr_in sin;

	KUNIT_EXPECT_EQ(test,
			rpc_pton(&init_net, "192.168.1.1",
				 strlen("192.168.1.1"),
				 (struct sockaddr *)&sin,
				 sizeof(sin) - 1), 0);
}

/* addr.c:148 also rejects buflen greater than INET_ADDRSTRLEN. */
static void pton_buflen_too_long(struct kunit *test)
{
	static const char overlong[] = "255.255.255.25555";
	struct sockaddr_storage ss;

	KUNIT_ASSERT_GT(test, strlen(overlong), (size_t)INET_ADDRSTRLEN);

	memset(&ss, 0, sizeof(ss));
	KUNIT_EXPECT_EQ(test,
			rpc_pton(&init_net, overlong, strlen(overlong),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);
}

/* addr.c:138 returns 0 for anything that is not AF_INET or AF_INET6. */
static void ntop_unsupported_family(struct kunit *test)
{
	struct sockaddr_storage ss;
	char buf[ADDRBUF_LEN];

	memset(&ss, 0, sizeof(ss));
	ss.ss_family = AF_UNIX;

	KUNIT_EXPECT_EQ(test,
			rpc_ntop((struct sockaddr *)&ss, buf, sizeof(buf)), 0);
}

#if IS_ENABLED(CONFIG_IPV6)
/* A scope id is appended only for link-local addresses (addr.c:78). */
static void ntop_ipv6_scope_id_appended(struct kunit *test)
{
	struct sockaddr_storage ss;
	struct sockaddr_in6 *sin6;
	char buf[ADDRBUF_LEN];

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, "fe80::1", strlen("fe80::1"),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);

	sin6 = (struct sockaddr_in6 *)&ss;
	sin6->sin6_scope_id = 3;

	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_NE(test,
			rpc_ntop((struct sockaddr *)&ss, buf, sizeof(buf)), 0);
	KUNIT_EXPECT_STREQ(test, buf, "fe80::1%3");
}

/* A zero scope id must not produce a "%0" suffix (addr.c:80). */
static void ntop_ipv6_scope_zero_omitted(struct kunit *test)
{
	struct sockaddr_storage ss;
	char buf[ADDRBUF_LEN];

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, "fe80::1", strlen("fe80::1"),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);

	memset(buf, 0, sizeof(buf));
	KUNIT_ASSERT_NE(test,
			rpc_ntop((struct sockaddr *)&ss, buf, sizeof(buf)), 0);
	KUNIT_EXPECT_STREQ(test, buf, "fe80::1");
}

static void pton_ipv6_scope_numeric(struct kunit *test)
{
	static const char input[] = "fe80::1%3";
	struct sockaddr_storage ss;
	struct sockaddr_in6 *sin6;

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_EQ(test,
			rpc_pton(&init_net, input, strlen(input),
				 (struct sockaddr *)&ss, sizeof(ss)),
			sizeof(struct sockaddr_in6));

	sin6 = (struct sockaddr_in6 *)&ss;
	KUNIT_EXPECT_EQ(test, sin6->sin6_scope_id, 3u);
}

/* A scope id on a non-link-local address is rejected (addr.c:176). */
static void pton_ipv6_scope_rejected_for_global(struct kunit *test)
{
	static const char input[] = "2001:db8::1%3";
	struct sockaddr_storage ss;

	memset(&ss, 0, sizeof(ss));
	KUNIT_EXPECT_EQ(test,
			rpc_pton(&init_net, input, strlen(input),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);
}
#endif /* CONFIG_IPV6 */

/*
 * Universal addresses: a presentation address with the port appended as
 * ".hibyte.lobyte" (RFC 5665).
 */
struct uaddr_param {
	const char	*desc;
	const char	*presentation;
	unsigned short	port;
	const char	*uaddr;
};

static void uaddr_get_desc(const struct uaddr_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct uaddr_param uaddr_params[] = {
	{ "ipv4 nfs port",	"192.168.1.1",	2049,	"192.168.1.1.8.1" },
	{ "ipv4 port zero",	"192.168.1.1",	0,	"192.168.1.1.0.0" },
	{ "ipv4 port max",	"192.168.1.1",	65535,	"192.168.1.1.255.255" },
	{ "ipv4 rpcbind port",	"10.0.0.1",	111,	"10.0.0.1.0.111" },
#if IS_ENABLED(CONFIG_IPV6)
	{ "ipv6 nfs port",	"2001:db8::1",	2049,	"2001:db8::1.8.1" },
#endif
};

KUNIT_ARRAY_PARAM(uaddr, uaddr_params, uaddr_get_desc);

static void sockaddr2uaddr_case(struct kunit *test)
{
	const struct uaddr_param *param = test->param_value;
	struct sockaddr_storage ss;
	char *uaddr;

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, param->presentation,
				 strlen(param->presentation),
				 (struct sockaddr *)&ss, sizeof(ss)), 0);
	rpc_set_port((struct sockaddr *)&ss, param->port);

	uaddr = rpc_sockaddr2uaddr((struct sockaddr *)&ss, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, uaddr);
	KUNIT_EXPECT_STREQ(test, uaddr, param->uaddr);
	kfree(uaddr);
}

static void uaddr2sockaddr_case(struct kunit *test)
{
	const struct uaddr_param *param = test->param_value;
	struct sockaddr_storage ss;

	memset(&ss, 0, sizeof(ss));
	KUNIT_ASSERT_NE(test,
			rpc_uaddr2sockaddr(&init_net, param->uaddr,
					   strlen(param->uaddr),
					   (struct sockaddr *)&ss,
					   sizeof(ss)), 0);

	KUNIT_EXPECT_EQ(test, rpc_get_port((struct sockaddr *)&ss),
			param->port);
}

/* sockaddr -> uaddr -> sockaddr must preserve both address and port. */
static void uaddr_roundtrip_case(struct kunit *test)
{
	const struct uaddr_param *param = test->param_value;
	struct sockaddr_storage first, second;
	char *uaddr;

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));

	KUNIT_ASSERT_NE(test,
			rpc_pton(&init_net, param->presentation,
				 strlen(param->presentation),
				 (struct sockaddr *)&first, sizeof(first)), 0);
	rpc_set_port((struct sockaddr *)&first, param->port);

	uaddr = rpc_sockaddr2uaddr((struct sockaddr *)&first, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, uaddr);

	KUNIT_EXPECT_NE(test,
			rpc_uaddr2sockaddr(&init_net, uaddr, strlen(uaddr),
					   (struct sockaddr *)&second,
					   sizeof(second)), 0);
	KUNIT_EXPECT_TRUE_MSG(test,
			      rpc_cmp_addr_port((struct sockaddr *)&first,
						(struct sockaddr *)&second),
			      "round trip through \"%s\" changed the address",
			      uaddr);
	kfree(uaddr);
}

static const struct invalid_param invalid_uaddrs[] = {
	{ "empty string",		"" },
	{ "no separator",		"nodots" },
	{ "only one separator",		"1.2" },
	{ "port octet overflow",	"192.168.1.1.256.1" },
	{ "port octet non-numeric",	"192.168.1.1.8.x" },
	{ "address not parseable",	"notanaddress.8.1" },
};

KUNIT_ARRAY_PARAM(invalid_uaddr, invalid_uaddrs, invalid_get_desc);

static void uaddr2sockaddr_invalid_case(struct kunit *test)
{
	const struct invalid_param *param = test->param_value;
	struct sockaddr_storage ss;

	memset(&ss, 0, sizeof(ss));
	KUNIT_EXPECT_EQ_MSG(test,
			    rpc_uaddr2sockaddr(&init_net, param->input,
					       strlen(param->input),
					       (struct sockaddr *)&ss,
					       sizeof(ss)), 0,
			    "rpc_uaddr2sockaddr() accepted \"%s\"",
			    param->input);
}

/* addr.c:318 rejects anything longer than RPCBIND_MAXUADDRLEN. */
static void uaddr2sockaddr_too_long(struct kunit *test)
{
	char overlong[RPCBIND_MAXUADDRLEN + 8];
	struct sockaddr_storage ss;

	memset(overlong, 'a', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';

	memset(&ss, 0, sizeof(ss));
	KUNIT_EXPECT_EQ(test,
			rpc_uaddr2sockaddr(&init_net, overlong,
					   strlen(overlong),
					   (struct sockaddr *)&ss,
					   sizeof(ss)), 0);
}

/*
 * Suites
 */

static struct kunit_case sunrpc_addr_pton_cases[] = {
	{
		.name			= "parse canonical address",
		.run_case		= pton_canonical_case,
		.generate_params	= canonical_addr_gen_params,
	},
	{
		.name			= "reject malformed address",
		.run_case		= pton_invalid_case,
		.generate_params	= invalid_presentation_gen_params,
	},
	KUNIT_CASE(pton_salen_too_small),
	KUNIT_CASE(pton_buflen_too_long),
#if IS_ENABLED(CONFIG_IPV6)
	KUNIT_CASE(pton_ipv6_scope_numeric),
	KUNIT_CASE(pton_ipv6_scope_rejected_for_global),
#endif
	{}
};

static struct kunit_suite sunrpc_addr_pton_suite = {
	.name		= "sunrpc-addr-pton",
	.test_cases	= sunrpc_addr_pton_cases,
};

static struct kunit_case sunrpc_addr_ntop_cases[] = {
	{
		.name			= "format canonical address",
		.run_case		= ntop_canonical_case,
		.generate_params	= canonical_addr_gen_params,
	},
	KUNIT_CASE(ntop_unsupported_family),
#if IS_ENABLED(CONFIG_IPV6)
	KUNIT_CASE(ntop_ipv6_scope_id_appended),
	KUNIT_CASE(ntop_ipv6_scope_zero_omitted),
#endif
	{}
};

static struct kunit_suite sunrpc_addr_ntop_suite = {
	.name		= "sunrpc-addr-ntop",
	.test_cases	= sunrpc_addr_ntop_cases,
};

static struct kunit_case sunrpc_addr_roundtrip_cases[] = {
	{
		.name			= "presentation round trip",
		.run_case		= roundtrip_case,
		.generate_params	= canonical_addr_gen_params,
	},
	{
		.name			= "universal address round trip",
		.run_case		= uaddr_roundtrip_case,
		.generate_params	= uaddr_gen_params,
	},
	{}
};

static struct kunit_suite sunrpc_addr_roundtrip_suite = {
	.name		= "sunrpc-addr-roundtrip",
	.test_cases	= sunrpc_addr_roundtrip_cases,
};

static struct kunit_case sunrpc_addr_uaddr_cases[] = {
	{
		.name			= "build universal address",
		.run_case		= sockaddr2uaddr_case,
		.generate_params	= uaddr_gen_params,
	},
	{
		.name			= "parse universal address",
		.run_case		= uaddr2sockaddr_case,
		.generate_params	= uaddr_gen_params,
	},
	{
		.name			= "reject malformed universal address",
		.run_case		= uaddr2sockaddr_invalid_case,
		.generate_params	= invalid_uaddr_gen_params,
	},
	KUNIT_CASE(uaddr2sockaddr_too_long),
	{}
};

static struct kunit_suite sunrpc_addr_uaddr_suite = {
	.name		= "sunrpc-addr-uaddr",
	.test_cases	= sunrpc_addr_uaddr_cases,
};

kunit_test_suites(&sunrpc_addr_pton_suite,
		  &sunrpc_addr_ntop_suite,
		  &sunrpc_addr_roundtrip_suite,
		  &sunrpc_addr_uaddr_suite);

MODULE_DESCRIPTION("Test SunRPC address conversion functions");
MODULE_LICENSE("GPL");
