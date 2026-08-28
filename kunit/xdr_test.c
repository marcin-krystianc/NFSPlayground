// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the SunRPC XDR codec in net/sunrpc/xdr.c and the inline
 * helpers in include/linux/sunrpc/xdr.h.
 *
 * XDR is the wire format underneath every NFS operation. Its defining
 * rule is that every object is padded out to a 4-byte boundary (RFC 4506),
 * and the padding must be zero-filled. Off-by-one errors around that
 * boundary are the classic XDR bug, so the alignment and padding cases
 * below are the point of this file rather than an afterthought.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/msg_prot.h>

#define XDRBUF_WORDS	64
#define XDRBUF_BYTES	(XDRBUF_WORDS * sizeof(__be32))

/*
 * Alignment and padding arithmetic
 */

struct align_param {
	const char	*desc;
	size_t		in;
	size_t		aligned;
	size_t		pad;
};

static void align_get_desc(const struct align_param *param, char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

static const struct align_param align_params[] = {
	{ "0 bytes",	0,	0,	0 },
	{ "1 byte",	1,	4,	3 },
	{ "2 bytes",	2,	4,	2 },
	{ "3 bytes",	3,	4,	1 },
	{ "4 bytes",	4,	4,	0 },
	{ "5 bytes",	5,	8,	3 },
	{ "7 bytes",	7,	8,	1 },
	{ "8 bytes",	8,	8,	0 },
	{ "1023 bytes",	1023,	1024,	1 },
	{ "1024 bytes",	1024,	1024,	0 },
};

KUNIT_ARRAY_PARAM(align, align_params, align_get_desc);

static void xdr_align_size_case(struct kunit *test)
{
	const struct align_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, xdr_align_size(param->in), param->aligned,
			    "input %zu", param->in);
}

static void xdr_pad_size_case(struct kunit *test)
{
	const struct align_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, xdr_pad_size(param->in), param->pad,
			    "input %zu", param->in);
}

/* An object plus its pad is always a whole number of XDR units. */
static void xdr_pad_completes_a_unit_case(struct kunit *test)
{
	const struct align_param *param = test->param_value;
	size_t total = param->in + xdr_pad_size(param->in);

	KUNIT_EXPECT_EQ(test, total % XDR_UNIT, 0UL);
	KUNIT_EXPECT_EQ(test, total, xdr_align_size(param->in));
}

static void xdr_quadlen_case(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(0), 0U);
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(1), 1U);
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(3), 1U);
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(4), 1U);
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(5), 2U);
	KUNIT_EXPECT_EQ(test, XDR_QUADLEN(8), 2U);
}

/*
 * Primitive encoders operating on a bare __be32 buffer
 */

/*
 * The padding after an opaque must be zero-filled, not left as whatever
 * the buffer held. Uninitialised padding would leak kernel memory onto
 * the wire, so this fills the buffer with 0xff first and checks the
 * encoder scrubs the tail.
 */
static void encode_opaque_fixed_zero_fills_pad(struct kunit *test)
{
	__be32 buf[XDRBUF_WORDS];
	const char data[] = { 'a', 'b', 'c' };
	unsigned char *bytes = (unsigned char *)buf;
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	end = xdr_encode_opaque_fixed(buf, data, sizeof(data));

	KUNIT_EXPECT_PTR_EQ(test, end, buf + 1);
	KUNIT_EXPECT_EQ(test, bytes[0], 'a');
	KUNIT_EXPECT_EQ(test, bytes[1], 'b');
	KUNIT_EXPECT_EQ(test, bytes[2], 'c');
	KUNIT_EXPECT_EQ_MSG(test, bytes[3], 0,
			    "pad byte was not zero-filled");
}

/* A zero-length object consumes nothing at all. */
static void encode_opaque_fixed_empty_advances_nothing(struct kunit *test)
{
	__be32 buf[XDRBUF_WORDS];
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	end = xdr_encode_opaque_fixed(buf, "x", 0);

	KUNIT_EXPECT_PTR_EQ(test, end, &buf[0]);
	/* Nothing was written, so the 0xff fill survives. */
	KUNIT_EXPECT_EQ(test, ((unsigned char *)buf)[0], 0xff);
}

/* A NULL source writes only the padding, leaving the body untouched. */
static void encode_opaque_fixed_null_pads_only(struct kunit *test)
{
	__be32 buf[XDRBUF_WORDS];
	unsigned char *bytes = (unsigned char *)buf;
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	end = xdr_encode_opaque_fixed(buf, NULL, 5);

	KUNIT_EXPECT_PTR_EQ(test, end, buf + 2);
	/* Bytes 0..4 are the (unwritten) body, 5..7 are the pad. */
	KUNIT_EXPECT_EQ(test, bytes[4], 0xff);
	KUNIT_EXPECT_EQ(test, bytes[5], 0);
	KUNIT_EXPECT_EQ(test, bytes[6], 0);
	KUNIT_EXPECT_EQ(test, bytes[7], 0);
}

/* xdr_encode_opaque() is the same thing with a length prefix. */
static void encode_opaque_writes_length_prefix(struct kunit *test)
{
	__be32 buf[XDRBUF_WORDS];
	const char data[] = { 'a', 'b', 'c' };
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	end = xdr_encode_opaque(buf, data, sizeof(data));

	KUNIT_EXPECT_EQ(test, be32_to_cpu(buf[0]), 3U);
	KUNIT_EXPECT_PTR_EQ(test, end, buf + 2);
}

static void encode_decode_string_roundtrip(struct kunit *test)
{
	static const char input[] = "hello";
	__be32 buf[XDRBUF_WORDS];
	unsigned int len = 0;
	char *out = NULL;
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	xdr_encode_string(buf, input);

	end = xdr_decode_string_inplace(buf, &out, &len, sizeof(input));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, end);
	KUNIT_EXPECT_EQ(test, len, (unsigned int)strlen(input));
	KUNIT_EXPECT_EQ(test, memcmp(out, input, strlen(input)), 0);
}

/* Decoding refuses a string longer than the caller's limit. */
static void decode_string_rejects_overlong(struct kunit *test)
{
	__be32 buf[XDRBUF_WORDS];
	unsigned int len = 0;
	char *out = NULL;

	memset(buf, 0, sizeof(buf));
	buf[0] = cpu_to_be32(100);

	KUNIT_EXPECT_PTR_EQ(test,
			    xdr_decode_string_inplace(buf, &out, &len, 10),
			    NULL);
}

static void encode_decode_netobj_roundtrip(struct kunit *test)
{
	static const u8 data[] = { 0xde, 0xad, 0xbe, 0xef, 0x01 };
	struct xdr_netobj in, out;
	__be32 buf[XDRBUF_WORDS];
	__be32 *end;

	memset(buf, 0xff, sizeof(buf));
	in.data = (u8 *)data;
	in.len = sizeof(data);

	end = xdr_encode_netobj(buf, &in);
	KUNIT_ASSERT_PTR_EQ(test, end, buf + 3);

	memset(&out, 0, sizeof(out));
	end = xdr_decode_netobj(buf, &out);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, end);
	KUNIT_EXPECT_EQ(test, out.len, in.len);
	KUNIT_EXPECT_EQ(test, memcmp(out.data, in.data, in.len), 0);
}

/* A netobj longer than XDR_MAX_NETOBJ is rejected rather than trusted. */
static void decode_netobj_rejects_oversized(struct kunit *test)
{
	struct xdr_netobj out;
	__be32 buf[XDRBUF_WORDS];

	memset(buf, 0, sizeof(buf));
	buf[0] = cpu_to_be32(XDR_MAX_NETOBJ + 1);

	KUNIT_EXPECT_PTR_EQ(test, xdr_decode_netobj(buf, &out), NULL);
}

/*
 * xdr_stream encode and decode
 */

struct xdr_test_ctx {
	__be32		buf[XDRBUF_WORDS];
	struct xdr_buf	xbuf;
	struct xdr_stream stream;
};

static struct xdr_test_ctx *xdr_ctx_new(struct kunit *test)
{
	struct xdr_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	return ctx;
}

/*
 * xdr_buf_init() sets head[0].iov_len to the whole buffer, which means
 * "this much is already encoded". For an empty buffer to encode into it
 * must be reset to zero, otherwise xdr_init_encode() places xdr->p at the
 * end of the buffer and trips its own BUG_ON(p < xdr->p).
 */
static void xdr_ctx_start_encode(struct xdr_test_ctx *ctx)
{
	xdr_buf_init(&ctx->xbuf, ctx->buf, XDRBUF_BYTES);
	ctx->xbuf.head[0].iov_len = 0;
	xdr_init_encode(&ctx->stream, &ctx->xbuf, ctx->buf, NULL);
}

static void xdr_ctx_start_decode(struct xdr_test_ctx *ctx, size_t len)
{
	ctx->xbuf.head[0].iov_base = ctx->buf;
	ctx->xbuf.head[0].iov_len = len;
	ctx->xbuf.len = len;
	xdr_init_decode(&ctx->stream, &ctx->xbuf, ctx->buf, NULL);
}

static void stream_u32_roundtrip(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got = 0;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test, xdr_stream_encode_u32(&ctx->stream, 0xdeadbeef),
			(ssize_t)sizeof(__u32));
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&ctx->stream), 4U);

	xdr_ctx_start_decode(ctx, 4);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, got, 0xdeadbeefU);
}

static void stream_u64_roundtrip(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u64 got = 0;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test,
			xdr_stream_encode_u64(&ctx->stream, 0x1122334455667788ULL),
			(ssize_t)sizeof(__u64));
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&ctx->stream), 8U);

	xdr_ctx_start_decode(ctx, 8);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u64(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, got, 0x1122334455667788ULL);
}

/*
 * An opaque of 5 bytes must occupy 4 (length) + 8 (padded body) on the
 * wire, not 4 + 5. This is the alignment rule applied through the stream
 * API rather than the primitive one.
 */
static void stream_opaque_consumes_padded_length(struct kunit *test)
{
	static const char data[] = { 1, 2, 3, 4, 5 };
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test,
			xdr_stream_encode_opaque(&ctx->stream, data,
						 sizeof(data)),
			(ssize_t)(sizeof(__u32) + 8));
	KUNIT_EXPECT_EQ(test, xdr_stream_pos(&ctx->stream), 12U);
}

static void stream_opaque_fixed_roundtrip(struct kunit *test)
{
	static const char data[] = { 'w', 'x', 'y', 'z', '!' };
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	char got[sizeof(data)];

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test,
			xdr_stream_encode_opaque_fixed(&ctx->stream, data,
						       sizeof(data)),
			(ssize_t)xdr_align_size(sizeof(data)));

	memset(got, 0, sizeof(got));
	xdr_ctx_start_decode(ctx, xdr_align_size(sizeof(data)));
	KUNIT_ASSERT_EQ(test,
			xdr_stream_decode_opaque_fixed(&ctx->stream, got,
						       sizeof(got)),
			(ssize_t)sizeof(got));
	KUNIT_EXPECT_EQ(test, memcmp(got, data, sizeof(data)), 0);
}

static void stream_bool_roundtrip(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got = 0;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test, xdr_stream_encode_bool(&ctx->stream, 1),
			(int)XDR_UNIT);

	xdr_ctx_start_decode(ctx, 4);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, got, 1U);
}

static void stream_uint32_array_roundtrip(struct kunit *test)
{
	static const __u32 values[] = { 1, 2, 3, 0xffffffff };
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got[ARRAY_SIZE(values)];
	unsigned int i;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test,
			xdr_stream_encode_uint32_array(&ctx->stream, values,
						       ARRAY_SIZE(values)),
			(ssize_t)((ARRAY_SIZE(values) + 1) * sizeof(__u32)));

	memset(got, 0, sizeof(got));
	xdr_ctx_start_decode(ctx, (ARRAY_SIZE(values) + 1) * sizeof(__u32));
	KUNIT_ASSERT_EQ(test,
			xdr_stream_decode_uint32_array(&ctx->stream, got,
						       ARRAY_SIZE(got)),
			(ssize_t)ARRAY_SIZE(values));

	for (i = 0; i < ARRAY_SIZE(values); i++)
		KUNIT_EXPECT_EQ_MSG(test, got[i], values[i], "element %u", i);
}

/*
 * A short array must zero the unused tail of the caller's buffer rather
 * than leaving stale values behind.
 */
static void stream_uint32_array_zeroes_tail(struct kunit *test)
{
	static const __u32 values[] = { 7, 8 };
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got[4];

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_GT(test,
			xdr_stream_encode_uint32_array(&ctx->stream, values,
						       ARRAY_SIZE(values)),
			(ssize_t)0);

	memset(got, 0xff, sizeof(got));
	xdr_ctx_start_decode(ctx, (ARRAY_SIZE(values) + 1) * sizeof(__u32));
	KUNIT_ASSERT_EQ(test,
			xdr_stream_decode_uint32_array(&ctx->stream, got,
						       ARRAY_SIZE(got)),
			(ssize_t)ARRAY_SIZE(values));

	KUNIT_EXPECT_EQ(test, got[0], 7U);
	KUNIT_EXPECT_EQ(test, got[1], 8U);
	KUNIT_EXPECT_EQ_MSG(test, got[2], 0U, "tail not zeroed");
	KUNIT_EXPECT_EQ_MSG(test, got[3], 0U, "tail not zeroed");
}

/* Present/absent discriminators are a single word, 1 or 0. */
static void stream_item_present_absent(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got = 0;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_EQ(test, xdr_stream_encode_item_present(&ctx->stream),
			(ssize_t)XDR_UNIT);
	KUNIT_ASSERT_EQ(test, xdr_stream_encode_item_absent(&ctx->stream),
			(int)XDR_UNIT);

	xdr_ctx_start_decode(ctx, 8);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, got, 1U);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, got, 0U);
}

/*
 * Overflow handling: the codec must refuse rather than run off the end.
 */

static void stream_decode_past_end_is_rejected(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	__u32 got = 0;

	xdr_ctx_start_encode(ctx);
	KUNIT_ASSERT_GT(test, xdr_stream_encode_u32(&ctx->stream, 1),
			(ssize_t)0);

	/* Only one word is readable; the second read must fail. */
	xdr_ctx_start_decode(ctx, 4);
	KUNIT_ASSERT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)0);
	KUNIT_EXPECT_EQ(test, xdr_stream_decode_u32(&ctx->stream, &got),
			(ssize_t)-EBADMSG);
}

static void stream_inline_decode_past_end_returns_null(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);

	xdr_ctx_start_decode(ctx, 8);
	KUNIT_EXPECT_PTR_EQ(test, xdr_inline_decode(&ctx->stream, 32), NULL);
}

static void stream_encode_beyond_buffer_is_rejected(struct kunit *test)
{
	struct xdr_test_ctx *ctx = xdr_ctx_new(test);
	unsigned int i;
	ssize_t ret = 0;

	xdr_ctx_start_encode(ctx);

	/*
	 * Write past capacity: the buffer holds exactly XDRBUF_WORDS words,
	 * so the overflow only shows up on the word after that.
	 */
	for (i = 0; i < XDRBUF_WORDS + 4; i++) {
		ret = xdr_stream_encode_u32(&ctx->stream, i);
		if (ret < 0)
			break;
	}

	KUNIT_EXPECT_LE_MSG(test, (size_t)i, (size_t)XDRBUF_WORDS,
			    "accepted %u words into a %u word buffer",
			    i, (unsigned int)XDRBUF_WORDS);

	KUNIT_EXPECT_EQ_MSG(test, ret, (ssize_t)-EMSGSIZE,
			    "encoder wrote past the end of the buffer");
}

/*
 * xdr_buf subrange extraction
 *
 * An xdr_buf is head + pages + tail. xdr_buf_subsegment() carves a
 * [base, base+len) window out of that, which is interval arithmetic
 * across three regions and exactly where off-by-one errors live. These
 * tests use a head-only buffer, so the window maths stays observable
 * without needing real pages.
 */

static struct xdr_buf *linear_buf(struct kunit *test, void *mem, size_t len)
{
	struct xdr_buf *buf = kunit_kzalloc(test, sizeof(*buf), GFP_KERNEL);
	struct kvec *iov = kunit_kzalloc(test, sizeof(*iov), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, iov);

	iov->iov_base = mem;
	iov->iov_len = len;
	xdr_buf_from_iov(iov, buf);
	return buf;
}

static void subsegment_whole_buffer(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_ASSERT_EQ(test, xdr_buf_subsegment(buf, &sub, 0, sizeof(mem)), 0);
	KUNIT_EXPECT_EQ(test, sub.len, (unsigned int)sizeof(mem));
	KUNIT_EXPECT_EQ(test, sub.head[0].iov_len, sizeof(mem));
	KUNIT_EXPECT_PTR_EQ(test, sub.head[0].iov_base, (void *)mem);
}

static void subsegment_offset_window(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_ASSERT_EQ(test, xdr_buf_subsegment(buf, &sub, 16, 16), 0);
	KUNIT_EXPECT_EQ(test, sub.len, 16U);
	KUNIT_EXPECT_EQ(test, sub.head[0].iov_len, 16UL);
	KUNIT_EXPECT_PTR_EQ(test, sub.head[0].iov_base, (void *)(mem + 16));
}

/* A window ending exactly at the end of the buffer is in bounds. */
static void subsegment_window_to_end_is_valid(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_EXPECT_EQ(test, xdr_buf_subsegment(buf, &sub, 60, 4), 0);
	KUNIT_EXPECT_EQ(test, sub.len, 4U);
}

/* One byte past the end is not. */
static void subsegment_past_end_is_rejected(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_EXPECT_EQ(test, xdr_buf_subsegment(buf, &sub, 60, 5), -1);
}

static void subsegment_base_past_end_is_rejected(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_EXPECT_EQ(test, xdr_buf_subsegment(buf, &sub, 65, 1), -1);
}

/* A zero-length window at a valid offset is legal and empty. */
static void subsegment_empty_window(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));
	struct xdr_buf sub;

	memset(&sub, 0, sizeof(sub));
	KUNIT_ASSERT_EQ(test, xdr_buf_subsegment(buf, &sub, 32, 0), 0);
	KUNIT_EXPECT_EQ(test, sub.len, 0U);
	KUNIT_EXPECT_EQ(test, sub.head[0].iov_len, 0UL);
}

/* Trimming removes bytes from the end and shortens the reported length. */
static void buf_trim_shortens_from_the_end(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));

	xdr_buf_trim(buf, 16);
	KUNIT_EXPECT_EQ(test, buf->len, 48U);
	KUNIT_EXPECT_EQ(test, buf->head[0].iov_len, 48UL);
}

/* Trimming more than the buffer holds empties it rather than underflowing. */
static void buf_trim_clamps_at_empty(struct kunit *test)
{
	char mem[64];
	struct xdr_buf *buf = linear_buf(test, mem, sizeof(mem));

	xdr_buf_trim(buf, 999);
	KUNIT_EXPECT_EQ(test, buf->len, 0U);
	KUNIT_EXPECT_EQ(test, buf->head[0].iov_len, 0UL);
}

/*
 * Suites
 */

static struct kunit_case xdr_align_cases[] = {
	{
		.name			= "align size",
		.run_case		= xdr_align_size_case,
		.generate_params	= align_gen_params,
	},
	{
		.name			= "pad size",
		.run_case		= xdr_pad_size_case,
		.generate_params	= align_gen_params,
	},
	{
		.name			= "object plus pad fills whole units",
		.run_case		= xdr_pad_completes_a_unit_case,
		.generate_params	= align_gen_params,
	},
	KUNIT_CASE(xdr_quadlen_case),
	{}
};

static struct kunit_suite xdr_align_suite = {
	.name		= "sunrpc-xdr-align",
	.test_cases	= xdr_align_cases,
};

static struct kunit_case xdr_primitive_cases[] = {
	KUNIT_CASE(encode_opaque_fixed_zero_fills_pad),
	KUNIT_CASE(encode_opaque_fixed_empty_advances_nothing),
	KUNIT_CASE(encode_opaque_fixed_null_pads_only),
	KUNIT_CASE(encode_opaque_writes_length_prefix),
	KUNIT_CASE(encode_decode_string_roundtrip),
	KUNIT_CASE(decode_string_rejects_overlong),
	KUNIT_CASE(encode_decode_netobj_roundtrip),
	KUNIT_CASE(decode_netobj_rejects_oversized),
	{}
};

static struct kunit_suite xdr_primitive_suite = {
	.name		= "sunrpc-xdr-primitives",
	.test_cases	= xdr_primitive_cases,
};

static struct kunit_case xdr_stream_cases[] = {
	KUNIT_CASE(stream_u32_roundtrip),
	KUNIT_CASE(stream_u64_roundtrip),
	KUNIT_CASE(stream_opaque_consumes_padded_length),
	KUNIT_CASE(stream_opaque_fixed_roundtrip),
	KUNIT_CASE(stream_bool_roundtrip),
	KUNIT_CASE(stream_uint32_array_roundtrip),
	KUNIT_CASE(stream_uint32_array_zeroes_tail),
	KUNIT_CASE(stream_item_present_absent),
	{}
};

static struct kunit_suite xdr_stream_suite = {
	.name		= "sunrpc-xdr-stream",
	.test_cases	= xdr_stream_cases,
};

static struct kunit_case xdr_overflow_cases[] = {
	KUNIT_CASE(stream_decode_past_end_is_rejected),
	KUNIT_CASE(stream_inline_decode_past_end_returns_null),
	KUNIT_CASE(stream_encode_beyond_buffer_is_rejected),
	{}
};

static struct kunit_suite xdr_overflow_suite = {
	.name		= "sunrpc-xdr-overflow",
	.test_cases	= xdr_overflow_cases,
};

static struct kunit_case xdr_subseg_cases[] = {
	KUNIT_CASE(subsegment_whole_buffer),
	KUNIT_CASE(subsegment_offset_window),
	KUNIT_CASE(subsegment_window_to_end_is_valid),
	KUNIT_CASE(subsegment_past_end_is_rejected),
	KUNIT_CASE(subsegment_base_past_end_is_rejected),
	KUNIT_CASE(subsegment_empty_window),
	KUNIT_CASE(buf_trim_shortens_from_the_end),
	KUNIT_CASE(buf_trim_clamps_at_empty),
	{}
};

static struct kunit_suite xdr_subseg_suite = {
	.name		= "sunrpc-xdr-subsegment",
	.test_cases	= xdr_subseg_cases,
};

kunit_test_suites(&xdr_align_suite,
		  &xdr_primitive_suite,
		  &xdr_stream_suite,
		  &xdr_overflow_suite,
		  &xdr_subseg_suite);

MODULE_DESCRIPTION("Test SunRPC XDR encoding and decoding");
MODULE_LICENSE("GPL");
