// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/001 over a loopback NFS mount.
 *
 * generic/001 is the SGI data-integrity chain copier: build files of many
 * sizes with deterministic per-file content, repeatedly copy each through a
 * chain (f -> f.0 -> f.1 -> ...), collapse the chain to f.last, and compare
 * against the untouched original. Any silent corruption in create, write,
 * copy, rename or unlink shows up as a mismatch.
 *
 * The NFS deployment lives in xfstests_nfs_fixture.c; every operation here
 * is a real RPC (OPEN/READ/WRITE/RENAME/REMOVE) through fs/nfs, served by
 * fs/nfsd over loopback TCP.
 *
 * Deliberate deviations from the shell original: fill(1)'s 72-byte text
 * line format is replaced by a seeded PRNG byte stream (the line format
 * existed so diff(1) output was readable); _mark_iteration is dropped (dead
 * code upstream -- defined, never called); the awk rand() schedule is not
 * replicated bitwise, only the algorithm. The check here is stronger than
 * upstream's: each original is also compared against its regenerated
 * expected content, so corruption of the original itself is caught, not
 * just divergence of the copy.
 */

#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>
#include <linux/stringhash.h>
#include <linux/random.h>

#include "xfstests_nfs_fixture.h"

#define GEN001_ROOT	XFS_MNT "/kunit-generic001"
#define GEN001_CHUNK	4096

/* The default config table from tests/generic/001, verbatim. */
static struct gen001_entry {
	const char	*rel;
	u32		bytes;
	int		link;
} gen001_config[] = {
	{ "small",	10 },		{ "big",	102400 },
	{ "sub/small",	10 },		{ "sub/big",	102400 },
	{ "sub/a",	1 },		{ "sub/b",	2 },
	{ "sub/c",	4 },		{ "sub/d",	8 },
	{ "sub/e",	16 },		{ "sub/f",	32 },
	{ "sub/g",	64 },		{ "sub/h",	128 },
	{ "sub/i",	256 },		{ "sub/j",	512 },
	{ "sub/k",	1024 },		{ "sub/l",	2048 },
	{ "sub/m",	4096 },		{ "sub/n",	8192 },
	{ "sub/a00",	100 },		{ "sub/b00",	200 },
	{ "sub/c00",	400 },		{ "sub/d00",	800 },
	{ "sub/e00",	1600 },		{ "sub/f00",	3200 },
	{ "sub/g00",	6400 },		{ "sub/h00",	12800 },
	{ "sub/i00",	25600 },	{ "sub/j00",	51200 },
	{ "sub/k00",	102400 },	{ "sub/l00",	204800 },
	{ "sub/m00",	409600 },	{ "sub/n00",	819200 },
	{ "sub/a000",	1000 },		{ "sub/e000",	16000 },
	{ "sub/h000",	128000 },	{ "sub/k000",	1024000 },
};

#define GEN001_NFILES	ARRAY_SIZE(gen001_config)
#define GEN001_NCOPY	200	/* copies per chain step, as upstream */
#define GEN001_ITERS	5	/* iterations, as upstream */

/* Scratch shared by the helpers; cases run one at a time. */
static char gen001_path_a[160];
static char gen001_path_b[160];

static const char *gen001_path(char *buf, const char *rel, int link)
{
	if (link == -1)
		snprintf(buf, 160, GEN001_ROOT "/%s", rel);
	else if (link == -2)
		snprintf(buf, 160, GEN001_ROOT "/%s.last", rel);
	else
		snprintf(buf, 160, GEN001_ROOT "/%s.%d", rel, link);
	return buf;
}

/* fill(1)'s analog: content is a PRNG stream seeded from the file's name. */
static void gen001_seed_for(const char *rel, struct rnd_state *st)
{
	prandom_seed_state(st, full_name_hash(NULL, rel, strlen(rel)));
}

static void gen001_fill_chunk(struct rnd_state *st, u8 *buf, size_t len)
{
	size_t i;
	u32 r = 0;

	for (i = 0; i < len; i++) {
		if ((i & 3) == 0)
			r = prandom_u32_state(st);
		buf[i] = r >> ((i & 3) * 8);
	}
}

/* _setup: create one file with its deterministic content. */
static void gen001_write_file(struct kunit *test, const char *rel, u32 bytes)
{
	struct rnd_state st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;
	u32 left = bytes;

	buf = kunit_kmalloc(test, GEN001_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(gen001_path(gen001_path_a, rel, -1),
		      O_WRONLY | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "creating %s over NFS: %ld",
			       rel, PTR_ERR(f));

	gen001_seed_for(rel, &st);
	while (left) {
		size_t n = min_t(u32, left, GEN001_CHUNK);
		ssize_t written;

		gen001_fill_chunk(&st, buf, n);
		written = kernel_write(f, buf, n, &pos);
		KUNIT_ASSERT_EQ_MSG(test, written, (ssize_t)n,
				    "short write on %s", rel);
		left -= n;
	}
	filp_close(f, NULL);
}

/*
 * cp(1)'s analog. O_EXCL on the destination doubles as the
 * "%s.%d already present!" guard from the awk-generated script.
 */
static void gen001_copy(struct kunit *test, const char *src, const char *dst)
{
	struct file *in, *out;
	loff_t rpos = 0, wpos = 0;
	u8 *buf;
	ssize_t got;

	buf = kunit_kmalloc(test, GEN001_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* "%s missing!" guard */
	in = filp_open(src, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(in), "%s missing! (%ld)",
			       src, PTR_ERR(in));

	out = filp_open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(out), "%s already present? (%ld)",
			       dst, PTR_ERR(out));

	while ((got = kernel_read(in, buf, GEN001_CHUNK, &rpos)) > 0) {
		ssize_t written = kernel_write(out, buf, got, &wpos);

		KUNIT_ASSERT_EQ_MSG(test, written, got, "short write on %s", dst);
	}
	KUNIT_ASSERT_GE_MSG(test, got, 0, "read error on %s", src);

	filp_close(in, NULL);
	filp_close(out, NULL);
}

/*
 * cmp(1)'s analog, chunked. `rel` names the entry whose seed regenerates
 * the expected stream; `path` is the file that must carry that content.
 */
static void gen001_verify_content(struct kunit *test, const char *rel,
				  const char *path, u32 bytes)
{
	struct rnd_state st;
	struct file *f;
	loff_t pos = 0;
	u8 *want, *got;
	u32 off = 0;

	want = kunit_kmalloc(test, GEN001_CHUNK, GFP_KERNEL);
	got = kunit_kmalloc(test, GEN001_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, want);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	f = filp_open(path, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s vanished! (%ld)",
			       path, PTR_ERR(f));

	gen001_seed_for(rel, &st);
	while (off < bytes) {
		size_t n = min_t(u32, bytes - off, GEN001_CHUNK);
		ssize_t r = kernel_read(f, got, n, &pos);

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "short read on %s at %u", path, off);
		gen001_fill_chunk(&st, want, n);
		if (memcmp(want, got, n)) {
			size_t i;

			for (i = 0; i < n && want[i] == got[i]; i++)
				;
			filp_close(f, NULL);
			KUNIT_FAIL(test,
				   "corruption for %s at byte %zu (want %02x got %02x)",
				   path, (size_t)off + i, want[i], got[i]);
			return;
		}
		off += n;
	}
	/* the file must also not be longer than expected */
	KUNIT_EXPECT_EQ_MSG(test, kernel_read(f, got, 1, &pos), 0,
			    "%s is longer than its expected %u bytes",
			    path, bytes);
	filp_close(f, NULL);
}

/* One iteration of _chain + close, mirroring the awk program. */
static void gen001_chain(struct kunit *test, int iter)
{
	struct rnd_state pick;
	int i;

	prandom_seed_state(&pick, iter);

	for (i = 0; i < GEN001_NCOPY; i++) {
		struct gen001_entry *e =
			&gen001_config[prandom_u32_state(&pick) % GEN001_NFILES];

		gen001_path(gen001_path_a, e->rel, e->link ? e->link - 1 : -1);
		gen001_path(gen001_path_b, e->rel, e->link);
		gen001_copy(test, gen001_path_a, gen001_path_b);
		e->link++;
	}

	/* close every chain: mv f.N f.last, rm the intermediates */
	for (i = 0; i < GEN001_NFILES; i++) {
		struct gen001_entry *e = &gen001_config[i];
		int j;

		if (!e->link)
			continue;
		gen001_path(gen001_path_a, e->rel, e->link - 1);
		gen001_path(gen001_path_b, e->rel, -2);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_rename(gen001_path_a, gen001_path_b),
				    0, "rename %s -> .last failed", e->rel);
		for (j = 0; j < e->link - 1; j++)
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_unlink(gen001_path(gen001_path_a, e->rel, j)),
					    0, "unlink %s.%d failed", e->rel, j);
		e->link = 0;
	}
}

/* _check: originals intact, and .last identical where it exists. */
static void gen001_check(struct kunit *test)
{
	int i;

	for (i = 0; i < GEN001_NFILES; i++) {
		struct gen001_entry *e = &gen001_config[i];

		gen001_verify_content(test, e->rel,
				      gen001_path(gen001_path_a, e->rel, -1),
				      e->bytes);
		if (xfs_exists(gen001_path(gen001_path_b, e->rel, -2)))
			gen001_verify_content(test, e->rel, gen001_path_b,
					      e->bytes);
	}
}

/* Best-effort removal of every name the engine can have created. */
static void gen001_remove_tree(void)
{
	char buf[160];
	int i, j;

	for (i = 0; i < GEN001_NFILES; i++) {
		const char *rel = gen001_config[i].rel;

		xfs_unlink(gen001_path(buf, rel, -1));
		xfs_unlink(gen001_path(buf, rel, -2));
		for (j = 0; j < GEN001_NCOPY; j++)
			if (xfs_unlink(gen001_path(buf, rel, j)))
				break;	/* densely numbered: first hole ends it */
		gen001_config[i].link = 0;
	}
	xfs_rmdir(GEN001_ROOT "/sub");
	xfs_rmdir(GEN001_ROOT);
}

static void gen001_remove_tree_action(void *unused)
{
	gen001_remove_tree();
}

static void gen001_mkdirs(struct kunit *test)
{
	int err;

	err = xfs_mkdir(GEN001_ROOT);
	KUNIT_ASSERT_TRUE_MSG(test, err == 0 || err == -EEXIST,
			      "mkdir " GEN001_ROOT ": %d", err);
	err = xfs_mkdir(GEN001_ROOT "/sub");
	KUNIT_ASSERT_TRUE_MSG(test, err == 0 || err == -EEXIST,
			      "mkdir sub: %d", err);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  gen001_remove_tree_action,
						  NULL), 0);
}

/*
 * ---------------------------------------------------------------------
 * Cases
 * ---------------------------------------------------------------------
 */

/* The go/no-go probe: the fixture is up and a page round-trips over NFS. */
static void loopback_nfs_mounts(struct kunit *test)
{
	static const char probe[] = XFS_MNT "/probe";
	struct file *f;
	loff_t pos;
	u8 *wr, *rd;
	ssize_t n;

	KUNIT_ASSERT_TRUE_MSG(test, xfstests_nfs_mounted(),
			      "suite init did not reach the client mount");

	wr = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	rd = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, wr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);
	get_random_bytes(wr, PAGE_SIZE);

	f = filp_open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
			       "open over NFS failed: %ld", PTR_ERR(f));
	pos = 0;
	n = kernel_write(f, wr, PAGE_SIZE, &pos);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)PAGE_SIZE);

	f = filp_open(probe, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	pos = 0;
	n = kernel_read(f, rd, PAGE_SIZE, &pos);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)PAGE_SIZE);

	KUNIT_EXPECT_EQ_MSG(test, memcmp(wr, rd, PAGE_SIZE), 0,
			    "a page did not round-trip over the NFS mount");
	KUNIT_EXPECT_EQ(test, xfs_unlink(probe), 0);

	/* and the file must exist server-side, under the tmpfs export */
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(XFS_EXPORT "/probe"),
			       "probe file still present under the export after unlink");
}

/* Vacuity guard for every comparison below. */
static void fill_is_deterministic_per_name(struct kunit *test)
{
	struct rnd_state st;
	u8 *a, *b;

	a = kunit_kmalloc(test, 512, GFP_KERNEL);
	b = kunit_kmalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);

	gen001_seed_for("sub/a00", &st);
	gen001_fill_chunk(&st, a, 512);
	gen001_seed_for("sub/a00", &st);
	gen001_fill_chunk(&st, b, 512);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(a, b, 512), 0,
			    "the same name generated two different streams");

	gen001_seed_for("sub/b00", &st);
	gen001_fill_chunk(&st, b, 512);
	KUNIT_EXPECT_NE_MSG(test, memcmp(a, b, 512), 0,
			    "two different names generated the same stream");
}

/* The algorithm in miniature: one file, a 3-link chain, over NFS. */
static void one_chain_survives_copy_rename_unlink(struct kunit *test)
{
	static const char rel[] = "sub/m";	/* PAGE_SIZE-sized entry */
	int j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	gen001_mkdirs(test);

	gen001_write_file(test, rel, PAGE_SIZE + 1);

	for (j = 0; j < 3; j++)
		gen001_copy(test,
			    gen001_path(gen001_path_a, rel, j - 1),
			    gen001_path(gen001_path_b, rel, j));

	KUNIT_ASSERT_EQ(test,
			xfs_rename(gen001_path(gen001_path_a, rel, 2),
				   gen001_path(gen001_path_b, rel, -2)), 0);
	for (j = 0; j < 2; j++)
		KUNIT_ASSERT_EQ(test,
				xfs_unlink(gen001_path(gen001_path_a, rel, j)), 0);

	gen001_verify_content(test, rel, gen001_path(gen001_path_a, rel, -1),
			      PAGE_SIZE + 1);
	gen001_verify_content(test, rel, gen001_path(gen001_path_a, rel, -2),
			      PAGE_SIZE + 1);
}

/* The full generic/001 run. */
static void chains_preserve_data_across_five_iterations(struct kunit *test)
{
	int i, iter;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	gen001_mkdirs(test);

	kunit_info(test, "setup: %zu files over NFS", GEN001_NFILES);
	for (i = 0; i < GEN001_NFILES; i++)
		gen001_write_file(test, gen001_config[i].rel,
				  gen001_config[i].bytes);

	for (iter = 1; iter <= GEN001_ITERS; iter++) {
		kunit_info(test, "iter %d chain (%d copies)", iter,
			   GEN001_NCOPY);
		gen001_chain(test, iter);
		gen001_check(test);
	}
}

static int gen001_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void gen001_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case gen001_cases[] = {
	KUNIT_CASE(loopback_nfs_mounts),
	KUNIT_CASE(fill_is_deterministic_per_name),
	KUNIT_CASE(one_chain_survives_copy_rename_unlink),
	KUNIT_CASE_SLOW(chains_preserve_data_across_five_iterations),
	{}
};

static struct kunit_suite gen001_suite = {
	.name		= "xfstests/generic/001",
	.suite_init	= gen001_suite_init,
	.suite_exit	= gen001_suite_exit,
	.test_cases	= gen001_cases,
};

kunit_test_suites(&gen001_suite);

MODULE_DESCRIPTION("xfstests generic/001 over a loopback NFS mount");
MODULE_LICENSE("GPL");
