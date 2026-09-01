// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/022 over a loopback NFS mount: delayed-allocation collapse range.
 *
 * The fourth sibling of common/punch's collapse family. All four drive
 * _test_generic_punch with the same six arguments and differ only in
 * flags: 021 plain, 012 "-k", 022 "-d", 016 "-d -k". This is "-d": no fsync, fresh file per layout.
 *
 * The "-d" half clears sync_cmd, so there is no fsync after the writes.
 * Over NFS that is a materially different code path from the fsync
 * variants: the data sits dirty in the client's page cache and reaches
 * the server only when writeback runs -- in practice on close, where
 * nfs4_file_flush() calls nfs_wb_all(). This port therefore doubles as a
 * close-to-open consistency test: nothing here ever calls fsync, yet the
 * server must hold every byte once the file is closed. Measured, not
 * assumed -- the log line each case emits shows the server does not have
 * the data before the close and does after.
 *
 * There is no "-k": the file is recreated for every layout, so each is
 * built on a clean slate. That is the only thing separating this from
 * generic/016.
 *
 * Over NFS collapse itself has no protocol mapping (generic/021 pins the
 * bare EOPNOTSUPP), so what these ports carry over is the matrix of
 * layouts the operation is attempted on -- and, for the "-d" pair, the
 * writeback path that gets the layouts to the server in the first place.
 *
 * Deviations, all structural: the unwritten-extent cases (3, 5, 7, 8, 9,
 * 12, 13) cannot be built over NFS -- unwritten extents come from
 * fallocate KEEP_SIZE, which nfs42_fallocate() rejects -- and case 16
 * differs from 14 only in page-cache temperature, which cannot matter to
 * an operation the client refuses before touching the cache. Upstream
 * itself skips the EOF case for fcollapse. Sizes use upstream's
 * multiple=16 scaling: 64K/128K/192K/320K.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G_ROOT		XFS_MNT "/g022"
#define G_FILE		G_ROOT "/case"
#define G_SERVER_FILE	XFS_EXPORT "/g022/case"

#define G_4K		(64 * 1024)	/* upstream's $_4k with multiple=16 */
#define G_8K		(128 * 1024)
#define G_12K		(192 * 1024)
#define G_20K		(320 * 1024)

struct g_case {
	const char	*name;		/* upstream's numbered label */
	loff_t		size;
	struct { loff_t off; loff_t len; } writes[2];
	bool		prealloc;	/* falloc 0..size first */
	struct { loff_t off; loff_t len; } punch;	/* len 0 = none */
	struct { loff_t off; loff_t len; } collapse;
};

static const struct g_case g_cases[] = {
	{ "1. into a hole", G_20K,
	  { }, false, { }, { G_4K, G_8K } },
	{ "2. into allocated space", G_20K,
	  { { 0, G_20K } }, false, { }, { G_4K, G_8K } },
	{ "4. hole -> data", G_20K,
	  { { G_8K, G_8K } }, false, { }, { G_4K, G_8K } },
	{ "6. data -> hole", G_20K,
	  { { 0, G_8K } }, false, { }, { G_4K, G_8K } },
	{ "10. hole -> data -> hole", G_20K,
	  { { G_8K, G_4K } }, false, { }, { G_4K, G_12K } },
	{ "11. data -> hole -> data", G_20K,
	  { { 0, G_8K }, { G_12K, G_8K } }, true, { G_8K, G_4K }, { G_4K, G_12K } },
	{ "14. data -> hole @ 0", G_20K,
	  { { 0, G_20K } }, false, { }, { 0, G_8K } },
	{ "17. data -> hole in single block file", 4096,
	  { { 0, 4096 } }, false, { }, { 128, 128 } },
};

/*
 * The model: expected content and expected size. Both are reset per case, because there is no -k.
 */
static u8 *g_shadow;
static loff_t g_size;

static void g_remove_tree(void *unused)
{
	xfs_unlink(G_FILE);
	xfs_rmdir(G_ROOT);
}

/*
 * Build one layout. Note what is NOT here: any fsync. That is the "-d"
 * flag -- upstream clears sync_cmd, so the writes stay in the client's
 * page cache and reach the server only through writeback.
 */
static struct file *g_build(struct kunit *test, const struct g_case *c, int ci)
{
	struct file *f;
	u8 *buf;
	int w;

	/* no -k: every layout starts from a clean slate */
	xfs_unlink(G_FILE);
	memset(g_shadow, 0, G_20K);
	g_size = 0;

	buf = kunit_kmalloc(test, G_8K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G_FILE, O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* truncate: shrinking discards the tail, growing exposes zeros */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G_FILE, c->size), 0);
	if (c->size < g_size)
		memset(g_shadow + c->size, 0, g_size - c->size);
	else if (c->size > g_size)
		memset(g_shadow + g_size, 0, c->size - g_size);
	g_size = c->size;

	if (c->prealloc)
		KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, c->size), 0,
				    "case %s: ALLOCATE failed", c->name);

	for (w = 0; w < 2 && c->writes[w].len; w++) {
		loff_t off = c->writes[w].off;
		loff_t left = c->writes[w].len;

		while (left) {
			size_t n = min_t(loff_t, left, G_8K);
			loff_t pos = off;
			size_t i;

			for (i = 0; i < n; i++)
				buf[i] = (u8)(ci * 37 + ((off + i) >> 10));
			KUNIT_ASSERT_EQ(test, kernel_write(f, buf, n, &pos),
					(ssize_t)n);
			memcpy(g_shadow + off, buf, n);
			off += n;
			left -= n;
		}
	}
	/* deliberately no vfs_fsync() here: this is the "-d" variant */

	if (c->punch.len) {
		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
						  FALLOC_FL_KEEP_SIZE,
						  c->punch.off, c->punch.len),
				    0, "case %s: DEALLOCATE failed", c->name);
		memset(g_shadow + c->punch.off, 0, c->punch.len);
	}
	return f;
}

/* compare a whole file against the model */
static bool g_verify(struct kunit *test, const char *path, u8 *got,
		     const struct g_case *c, int ci, const char *which)
{
	ssize_t n = xfs_read_range(path, got, c->size, 0);
	int i;

	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)c->size,
			    "case %s: %s read returned %zd", c->name, which, n);
	for (i = 0; i < c->size; i++)
		if (got[i] != g_shadow[i]) {
			KUNIT_FAIL(test,
				   "case %s (#%d): %s byte %d is %02x, model says %02x",
				   c->name, ci, which, i, got[i], g_shadow[i]);
			return false;
		}
	return true;
}

static void delayed_collapse_noop_and_writeback_delivers(struct kunit *test)
{
	u8 *got;
	int ci;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g_remove_tree, NULL), 0);

	g_shadow = kunit_kzalloc(test, G_20K, GFP_KERNEL);
	got = kunit_kmalloc(test, G_20K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, g_shadow);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	/* upstream's one unconditional rm before the first layout */
	xfs_unlink(G_FILE);
	g_size = 0;

	for (ci = 0; ci < ARRAY_SIZE(g_cases); ci++) {
		const struct g_case *c = &g_cases[ci];
		struct kstat st;
		struct file *f;
		int err;

		f = g_build(test, c, ci);

		err = vfs_fallocate(f, FALLOC_FL_COLLAPSE_RANGE,
				    c->collapse.off, c->collapse.len);
		KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
				    "case %s: collapse returned %d, expected EOPNOTSUPP",
				    c->name, err);

		/*
		 * Report whether the server already has the layout. It does
		 * not, for every case that writes anything -- measured, and
		 * left in as a permanent diagnostic so the mechanism stays
		 * visible in the log rather than being asserted racily
		 * (spontaneous writeback would make an absence assertion
		 * flaky).
		 */
		{
			ssize_t pn = xfs_read_range(G_SERVER_FILE, got,
						    c->size, 0);

			kunit_info(test, "case %s: server has it before close: %s",
				   c->name,
				   (pn == (ssize_t)c->size &&
				    !memcmp(got, g_shadow, c->size)) ?
				   "yes" : "no");
		}

		/*
		 * Close, then look at the server with nothing NFS-side in
		 * between. The ordering is load-bearing: several innocuous
		 * operations flush dirty pages themselves, and any of them
		 * ahead of the server check would deliver the data and leave
		 * this asserting nothing.
		 *
		 *   - nfs_getattr() writes back when STATX_CTIME/MTIME are
		 *     requested (fs/nfs/inode.c:982), so the size check comes
		 *     after;
		 *   - nfs_file_read() -> nfs_revalidate_mapping() ->
		 *     nfs_sync_mapping() writes back before invalidating, so
		 *     reading through the client comes after too.
		 *
		 * That leaves filp_close() -> nfs4_file_flush()
		 * (fs/nfs/nfs4file.c:111, the v4 fop -- not nfs_file_flush(),
		 * which serves v2/v3). Confirmed by mutation: making
		 * nfs4_file_flush() return early fails this port on the very
		 * first case that writes anything, while generic/012 -- same
		 * layouts, but with fsync -- still passes. That contrast is
		 * the whole reason the "-d" siblings are worth porting
		 * separately.
		 */
		filp_close(f, NULL);

		if (!g_verify(test, G_SERVER_FILE, got, c, ci, "SERVER"))
			return;

		/* now the flushing operations are harmless */
		if (!g_verify(test, G_FILE, got, c, ci, "client"))
			return;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, c->size,
				    "case %s: the rejected collapse changed the size",
				    c->name);
	}
}

static int g_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g_cases_tab[] = {
	KUNIT_CASE(delayed_collapse_noop_and_writeback_delivers),
	{}
};

static struct kunit_suite g_suite = {
	.name		= "xfstests/generic/022",
	.suite_init	= g_suite_init,
	.suite_exit	= g_suite_exit,
	.test_cases	= g_cases_tab,
};

kunit_test_suites(&g_suite);

MODULE_DESCRIPTION("xfstests generic/022 over a loopback NFS mount");
MODULE_LICENSE("GPL");
