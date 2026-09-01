// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/031 over a loopback NFS mount: unaligned writes across a
 * refused collapse.
 *
 * Upstream writes two overlapping, deliberately unaligned ranges with two
 * fcollapse calls interleaved, then hexdumps before and after cycling the
 * mount. It is hunting for partial pages left behind by collapse causing
 * invalidation or corruption.
 *
 * Over NFS there is no collapse: nfs42_fallocate() accepts only mode 0 and
 * PUNCH_HOLE|KEEP_SIZE (fs/nfs/nfs4file.c:228), so FALLOC_FL_COLLAPSE_RANGE
 * is EOPNOTSUPP. generic/021 already pins that bare rejection and generic/012
 * runs the rejection across a matrix of layouts, so what 031 adds here is
 * narrow and worth stating plainly: its offsets and lengths are not page
 * multiples. 012's layouts are all built from 64K units; 031 writes 55756
 * bytes at 185332 and 63394 bytes at 133228, so both ends of both writes land
 * mid-page, and the two ranges overlap. It is a second layout over the same
 * code path -- the same kind of addition generic/030 makes to generic/029 --
 * not a new mechanism.
 *
 * The interesting part over NFS is therefore not the collapse but the
 * writes: two overlapping unaligned WRITEs whose union must arrive at the
 * server intact, with the refused collapses in between changing nothing.
 *
 * Deviation: upstream's expected size is 196032, which is what the file
 * measures once both collapses have removed 45056 bytes. Here they are
 * refused, so the file keeps every byte and ends at 241088. The golden
 * hexdump is inverted for the same reason -- this is the "the op is refused,
 * so nothing may change" shape shared by the whole collapse family.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G031_ROOT	XFS_MNT "/g031"
#define G031_FILE	G031_ROOT "/testfile"
#define G031_SERVER	XFS_EXPORT "/g031/testfile"

/* upstream's offsets and lengths, unchanged -- none of them page-aligned */
#define G031_OFF1	185332
#define G031_LEN1	55756
#define G031_OFF2	133228
#define G031_LEN2	63394

#define G031_END	(G031_OFF1 + G031_LEN1)		/* 241088 */
#define G031_DATA	G031_OFF2			/* first written byte */
#define G031_FILL	0xcd

static void g031_remove_tree(void *unused)
{
	xfs_unlink(G031_FILE);
	xfs_rmdir(G031_ROOT);
}

static void g031_pwrite(struct kunit *test, struct file *f, u8 *buf,
			loff_t off, loff_t len)
{
	loff_t done = 0;

	memset(buf, G031_FILL, PAGE_SIZE);
	while (done < len) {
		size_t n = min_t(loff_t, len - done, PAGE_SIZE);
		loff_t pos = off + done;

		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, buf, n, &pos),
				    (ssize_t)n, "pwrite %lld at %lld", len,
				    off + done);
		done += n;
	}
}

static int g031_collapse(const char *path, loff_t off, loff_t len)
{
	struct file *f = filp_open(path, O_RDWR, 0);
	int err;

	if (IS_ERR(f))
		return PTR_ERR(f);
	err = vfs_fallocate(f, FALLOC_FL_COLLAPSE_RANGE, off, len);
	filp_close(f, NULL);
	return err;
}

/*
 * The whole file, as a range assertion: a hole up to the first written byte,
 * then the fill to EOF. This is the golden hexdump's content.
 */
static void g031_verify(struct kunit *test, const char *path, const char *which)
{
	struct kstat st;
	u8 *got;
	loff_t off;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G031_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G031_END,
			    "%s: size is %lld, expected %d -- a refused collapse must not resize",
			    which, st.size, G031_END);

	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	for (off = 0; off < G031_END; ) {
		size_t n = min_t(loff_t, G031_END - off, PAGE_SIZE);
		ssize_t r = xfs_read_range(path, got, n, off);
		size_t i;

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "%s: read at %lld returned %zd", which, off,
				    r);
		for (i = 0; i < n; i++) {
			u8 want = (off + (loff_t)i) < G031_DATA ? 0 : G031_FILL;

			if (got[i] != want) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   which, off + (loff_t)i, got[i], want);
				return;
			}
		}
		off += n;
	}
}

static void unaligned_writes_survive_a_refused_collapse(struct kunit *test)
{
	struct file *f;
	u8 *buf;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G031_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g031_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G031_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	/* upstream's four operations, in order */
	g031_pwrite(test, f, buf, G031_OFF1, G031_LEN1);

	err = g031_collapse(G031_FILE, 28672, 40960);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "first collapse returned %d, expected EOPNOTSUPP",
			    err);

	/* overlaps the first write and starts before it, both ends unaligned */
	g031_pwrite(test, f, buf, G031_OFF2, G031_LEN2);

	err = g031_collapse(G031_FILE, 0, 4096);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "second collapse returned %d, expected EOPNOTSUPP",
			    err);

	/* the close flushes; then check the client's view and the server's */
	filp_close(f, NULL);

	g031_verify(test, G031_FILE, "client");
	g031_verify(test, G031_SERVER, "SERVER");
}

static int g031_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g031_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g031_cases[] = {
	KUNIT_CASE(unaligned_writes_survive_a_refused_collapse),
	{}
};

static struct kunit_suite g031_suite = {
	.name		= "xfstests/generic/031",
	.suite_init	= g031_suite_init,
	.suite_exit	= g031_suite_exit,
	.test_cases	= g031_cases,
};

kunit_test_suites(&g031_suite);

MODULE_DESCRIPTION("xfstests generic/031 over a loopback NFS mount");
MODULE_LICENSE("GPL");
