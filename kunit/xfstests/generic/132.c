// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/132 over a loopback NFS mount: aligned vector rw.
 *
 * Upstream runs thirteen xfs_io invocations against one file. Each writes
 * a few pattern-filled blocks and reads blocks back; the block size grows
 * from 512 bytes to 10 MiB, and the file ends at 100 MiB. Later stages
 * overwrite ranges earlier ones wrote, and every read is compared against
 * a golden hexdump. The small stages are sub-page writes at adjacent
 * offsets, where a partial-page writeback bug in the client shows; the
 * large ones are multi-page and multi-wsize transfers.
 *
 * The stages below are upstream's, in its order: the same opens (only the
 * first truncates), the same writes and the same reads, including the
 * repeated "pread -v 8192 8192" in the fifth. Deviations: the golden
 * hexdumps are replaced by byte-exact expectations computed from the
 * writes so far; transfers larger than 1 MiB are issued as 1 MiB calls;
 * and the export is 128 MiB instead of the fixture's 64 MiB default, so
 * the 100 MiB file fits.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>

#include "xfstests_nfs_fixture.h"

#define G132_ROOT	XFS_MNT "/g132"
#define G132_FILE	G132_ROOT "/aligned_vector_rw"
#define G132_CHUNK	(1024 * 1024)
#define G132_EXPORT	"size=134217728,nr_inodes=32768"
#define G132_MAX_WRITES	64

struct g132_write {
	u8 val;
	loff_t off;
	size_t len;
};

struct g132_stage {
	bool trunc;
	const struct g132_write *w;
	int nw;
	size_t rlen;
	const loff_t *r;
	int nr;
};

#define G132_W(...)	(const struct g132_write[]){ __VA_ARGS__ }
#define G132_R(...)	(const loff_t[]){ __VA_ARGS__ }
#define G132_STAGE(t, ws, l, rs)					\
	{ .trunc = t, .w = ws, .nw = ARRAY_SIZE((ws)),			\
	  .rlen = l, .r = rs, .nr = ARRAY_SIZE((rs)) }

static const struct g132_stage g132_stages[] = {
	G132_STAGE(true,
	      G132_W({ 0x63, 0, 512 }, { 0x64, 512, 512 }, { 0x65, 1024, 512 },
		{ 0x66, 1536, 512 }, { 0x67, 2048, 512 }, { 0x68, 2560, 512 },
		{ 0x69, 3072, 512 }, { 0x6A, 3584, 512 }),
	      512, G132_R(0, 512, 1024, 1536, 2048, 2560, 3072, 3584)),
	G132_STAGE(false,
	      G132_W({ 0x63, 4096, 1024 }, { 0x6B, 5120, 1024 },
		{ 0x6C, 6144, 1024 }, { 0x6D, 7168, 1024 }),
	      1024, G132_R(0, 1024, 2048, 3072, 4096, 5120, 6144, 7168)),
	G132_STAGE(false,
	      G132_W({ 0x6E, 8192, 2048 }, { 0x6F, 10240, 2048 }),
	      2048, G132_R(0, 2048, 4096, 6144, 8192, 10240)),
	G132_STAGE(false,
	      G132_W({ 0x70, 12288, 4096 }),
	      4096, G132_R(0, 4096, 8192, 12288)),
	G132_STAGE(false,
	      G132_W({ 0x71, 16384, 8192 }, { 0x72, 24576, 8192 }),
	      8192, G132_R(0, 8192, 8192, 16384)),
	G132_STAGE(false,
	      G132_W({ 0x73, 32768, 16384 }, { 0x74, 49152, 16384 }),
	      16384, G132_R(0, 16384, 32768, 49152)),
	G132_STAGE(false,
	      G132_W({ 0x75, 65536, 32768 }, { 0x76, 98304, 32768 }),
	      32768, G132_R(0, 32768, 65536, 98304)),
	G132_STAGE(false,
	      G132_W({ 0x76, 131072, 65536 }, { 0x77, 196608, 65536 }),
	      65536, G132_R(0, 65536, 131072, 196608)),
	G132_STAGE(false,
	      G132_W({ 0x76, 262144, 131072 }, { 0x77, 393216, 131072 }),
	      131072, G132_R(0, 131072, 262144, 393216)),
	G132_STAGE(false,
	      G132_W({ 0x76, 524288, 524288 }, { 0x77, 1048576, 524288 }),
	      524288, G132_R(0, 524288, 1048576)),
	G132_STAGE(false,
	      G132_W({ 0x32, 1048576, 1048576 },
		{ 0x33, 2097152, 1048576 },
		{ 0x34, 3145728, 1048576 },
		{ 0x35, 4194304, 1048576 },
		{ 0x36, 5242880, 1048576 },
		{ 0x37, 6291456, 1048576 },
		{ 0x38, 7340032, 1048576 }, { 0x39, 8388608, 1048576 }),
	      1048576, G132_R(0, 1048576, 2097152, 3145728, 4194304, 5242880,
			6291456, 7340032, 8388608)),
	G132_STAGE(false,
	      G132_W({ 0x32, 1048576, 1048576 },
		{ 0x33, 2097152, 1048576 },
		{ 0x34, 3145728, 1048576 },
		{ 0x35, 4194304, 1048576 },
		{ 0x36, 5242880, 1048576 },
		{ 0x37, 6291456, 1048576 },
		{ 0x38, 7340032, 1048576 },
		{ 0x39, 8388608, 1048576 }, { 0x3A, 9437184, 1048576 }),
	      1048576, G132_R(0, 1048576, 2097152, 3145728, 4194304, 5242880,
			6291456, 7340032, 8388608, 9437184)),
	G132_STAGE(false,
	      G132_W({ 0x92, 10485760, 10485760 },
		{ 0x93, 20971520, 10485760 },
		{ 0x94, 31457280, 10485760 },
		{ 0x95, 41943040, 10485760 },
		{ 0x96, 52428800, 10485760 },
		{ 0x97, 62914560, 10485760 },
		{ 0x98, 73400320, 10485760 },
		{ 0x99, 83886080, 10485760 },
		{ 0x9A, 94371840, 10485760 }),
	      10485760, G132_R(0, 10485760, 20971520, 31457280, 41943040, 52428800,
			62914560, 73400320, 83886080, 94371840)),
};

/* every write issued so far, in order: what the reads are checked against */
struct g132_model {
	const struct g132_write *w[G132_MAX_WRITES];
	int n;
};

/* the byte at x, and where the run of that byte ends (exclusive) */
static u8 g132_byte_at(const struct g132_model *m, loff_t x, loff_t *end)
{
	u8 val = 0;
	bool found = false;
	int i;

	*end = LLONG_MAX;
	for (i = m->n - 1; i >= 0; i--) {
		const struct g132_write *w = m->w[i];
		loff_t wend = w->off + w->len;

		if (!found && x >= w->off && x < wend) {
			val = w->val;
			found = true;
		}
		if (w->off > x)
			*end = min(*end, w->off);
		if (wend > x)
			*end = min(*end, wend);
	}
	return val;
}

static void g132_remove_tree(void *unused)
{
	xfs_unlink(G132_FILE);
	xfs_rmdir_settled(G132_ROOT);
}

static void g132_write(struct kunit *test, struct file *f,
		       const struct g132_write *w, u8 *buf, int stage)
{
	loff_t pos = w->off;
	size_t done = 0;

	memset(buf, w->val, min(w->len, (size_t)G132_CHUNK));
	while (done < w->len) {
		size_t n = min(w->len - done, (size_t)G132_CHUNK);
		ssize_t r = kernel_write(f, buf, n, &pos);

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "stage %d: writing %zu bytes at %lld returned %zd",
				    stage, n, pos, r);
		done += n;
	}
}

static void g132_read(struct kunit *test, struct file *f,
		      const struct g132_model *m, loff_t off, size_t len,
		      u8 *buf, int stage)
{
	size_t done = 0;

	while (done < len) {
		size_t n = min(len - done, (size_t)G132_CHUNK);
		loff_t pos = off + done;
		loff_t x = pos, stop = pos + n;
		ssize_t r = kernel_read(f, buf, n, &pos);

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "stage %d: reading %zu bytes at %lld returned %zd",
				    stage, n, x, r);
		while (x < stop) {
			loff_t end;
			u8 want = g132_byte_at(m, x, &end);
			u8 *bad;

			end = min(end, stop);
			bad = memchr_inv(buf + (x - (stop - n)), want, end - x);
			if (bad) {
				KUNIT_FAIL(test,
					   "stage %d: byte %lld is %02x, expected %02x",
					   stage, stop - n + (bad - buf), *bad,
					   want);
				return;
			}
			x = end;
		}
		done += n;
	}
}

static void aligned_vector_rw(struct kunit *test)
{
	struct g132_model *m;
	u8 *buf;
	int s, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G132_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g132_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G132_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, m);

	for (s = 0; s < ARRAY_SIZE(g132_stages); s++) {
		const struct g132_stage *st = &g132_stages[s];
		int flags = O_RDWR | O_CREAT | (st->trunc ? O_TRUNC : 0);
		struct file *f = filp_open(G132_FILE, flags, 0644);

		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "stage %d: open: %ld",
				       s + 1, PTR_ERR(f));
		if (st->trunc)
			m->n = 0;
		for (i = 0; i < st->nw; i++) {
			KUNIT_ASSERT_LT(test, m->n, G132_MAX_WRITES);
			g132_write(test, f, &st->w[i], buf, s + 1);
			m->w[m->n++] = &st->w[i];
		}
		for (i = 0; i < st->nr; i++)
			g132_read(test, f, m, st->r[i], st->rlen, buf, s + 1);
		filp_close(f, NULL);
	}
}

static int g132_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G132_EXPORT);
	return xfstests_nfs_get();
}

static void g132_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g132_cases[] = {
	KUNIT_CASE_SLOW(aligned_vector_rw),
	{}
};

static struct kunit_suite g132_suite = {
	.name		= "xfstests/generic/132",
	.suite_init	= g132_suite_init,
	.suite_exit	= g132_suite_exit,
	.test_cases	= g132_cases,
};

kunit_test_suites(&g132_suite);

MODULE_DESCRIPTION("xfstests generic/132 over a loopback NFS mount");
MODULE_LICENSE("GPL");
