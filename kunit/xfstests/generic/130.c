// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/130 over a loopback NFS mount: a battery of buffered
 * and direct I/O patterns.
 *
 * Upstream runs one xfs_io invocation per scenario and compares the
 * hexdumps against a golden file. The scenarios, in its order:
 *
 *	end-of-file zeroing with O_DIRECT
 *	reading a hole made by truncate
 *	buffered and direct writes to the same file, read back buffered
 *	direct reads and writes: 64 KiB then 6553600 bytes, and a second
 *	file of 64 KiB then 128 KiB, each read back direct in the same
 *	open and again after a reopen
 *	the "FSB edge" case: truncate away, re-extend, write across 64k
 *	O_TRUNC on five files, before and after they hold "Test"
 *	O_APPEND ignoring the offset it is given
 *	single-byte reads, then single-byte writes either side of a page
 *	boundary and at ~10GB offsets, first without and then with O_SYNC
 *
 * Over NFS the interesting ones are the mixed cases: a direct write has
 * to invalidate whatever the client had cached for its range, and a
 * buffered read afterwards has to fetch it from the server rather than
 * serve the pre-write page. The hole cases check that the client
 * zero-fills rather than returning whatever the page held.
 *
 * Each xfs_io invocation is one open here, and each of its commands is
 * the same operation on that open file, at upstream's offsets and sizes.
 * Deviations: the golden hexdumps are replaced by byte-exact
 * expectations; transfers larger than 640 KiB are issued as 640 KiB
 * calls, the size of the kmalloc'd buffer the direct I/O helpers need;
 * O_LARGEFILE is passed by hand, since an in-kernel open does not get
 * force_o_largefile(). The coherency scenario also checks the 0x78 range
 * past upstream's 9000-byte read.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G130_ROOT	XFS_MNT "/g130"
#define G130_CHUNK	(640 * 1024)
/* the ~10GB offset of upstream's small-vector writes */
#define G130_FAR	10000000000LL

static const char * const g130_files[] = {
	"eof-zeroing_direct", "blackhole", "buff_direct_coherency",
	"direct_io", "async_direct_io", "fsb_edge_test",
	"0", "1", "2", "3", "4", "append", "small_vector_async",
};

static void g130_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g130_files); i++) {
		snprintf(path, sizeof(path), G130_ROOT "/%s", g130_files[i]);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G130_ROOT);
}

static int g130_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G130_ROOT), 0);
	return kunit_add_action_or_reset(test, g130_remove_tree, NULL);
}

static struct file *g130_open(struct kunit *test, const char *path, int flags)
{
	struct file *f = filp_open(path, flags | O_LARGEFILE, 0644);

	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open %s: %ld", path,
			       PTR_ERR(f));
	return f;
}

/* write len bytes of val at off, through f */
static void g130_fill(struct kunit *test, struct file *f, bool direct, u8 val,
		      loff_t off, size_t len, u8 *scratch, const char *what)
{
	loff_t pos = off;
	size_t done = 0;

	while (done < len) {
		size_t n = min(len - done, (size_t)G130_CHUNK);
		ssize_t w;

		memset(scratch, val, n);
		w = direct ? xfs_direct_write(f, scratch, n, &pos)
			   : kernel_write(f, scratch, n, &pos);
		KUNIT_ASSERT_EQ_MSG(test, w, (ssize_t)n,
				    "%s: writing %zu bytes at %lld returned %zd",
				    what, n, pos, w);
		done += n;
	}
}

/*
 * The expected contents of a read: consecutive ranges starting at the
 * read's offset, each ending (exclusive) at end and holding val.
 */
struct g130_seg {
	loff_t end;
	u8 val;
};

#define G130_SEGS(...)	((const struct g130_seg[]){ __VA_ARGS__ })

/* read [off, off+len) through f and check it against seg */
static void g130_expect(struct kunit *test, struct file *f, bool direct,
			loff_t off, size_t len, const struct g130_seg *seg,
			u8 *scratch, const char *what)
{
	size_t done = 0;

	while (done < len) {
		size_t n = min(len - done, (size_t)G130_CHUNK);
		loff_t pos = off + done;
		ssize_t r = direct ? xfs_direct_read(f, scratch, n, &pos)
				   : kernel_read(f, scratch, n, &pos);
		int i;

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "%s: read at %lld returned %zd", what,
				    off + done, r);
		for (i = 0; i < n; i++) {
			loff_t at = off + done + i;

			while (at >= seg->end)
				seg++;
			if (scratch[i] != seg->val) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   what, at, scratch[i], seg->val);
				return;
			}
		}
		done += n;
	}
}

static void end_of_file_zeroing_with_direct_io(struct kunit *test)
{
	const char *path = G130_ROOT "/eof-zeroing_direct";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT);
	g130_fill(test, f, true, 0x63, 0, 65536, buf, "the 0x63 fill");
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 1), 0);
	g130_fill(test, f, true, 0x41, 65536, 65536, buf, "the 0x41 fill");

	g130_expect(test, f, true, 0, 131072,
		    G130_SEGS({ 1, 0x63 }, { 65536, 0x00 }, { 131072, 0x41 }),
		    buf, "the whole file");
	filp_close(f, NULL);
}

static void reading_a_hole_made_by_truncate(struct kunit *test)
{
	const char *path = G130_ROOT "/blackhole";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 8192), 0);
	g130_expect(test, f, false, 5000, 3000, G130_SEGS({ 8000, 0x00 }),
		    buf, "the hole");
	filp_close(f, NULL);
}

static void buffered_and_direct_writes_stay_coherent(struct kunit *test)
{
	const char *path = G130_ROOT "/buff_direct_coherency";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC);
	g130_fill(test, f, false, 0x41, 8000, 1000, buf, "buffered 0x41");
	g130_fill(test, f, false, 0x57, 4000, 1000, buf, "buffered 0x57");
	filp_close(f, NULL);

	f = g130_open(test, path, O_RDWR | O_DIRECT);
	g130_fill(test, f, true, 0x78, 20480, 4096, buf, "direct 0x78");
	g130_fill(test, f, true, 0x79, 4096, 4096, buf, "direct 0x79");
	filp_close(f, NULL);

	/* the direct writes came last, so they own their ranges */
	f = g130_open(test, path, O_RDWR);
	g130_expect(test, f, false, 0, 9000,
		    G130_SEGS({ 4000, 0x00 }, { 4096, 0x57 }, { 8192, 0x79 },
			      { 9000, 0x41 }),
		    buf, "the first 9000 bytes");
	g130_expect(test, f, false, 20480, 4096, G130_SEGS({ 24576, 0x78 }),
		    buf, "the 0x78 range");
	filp_close(f, NULL);
}

/*
 * One file of upstream's direct read/write scenario: 64 KiB of first,
 * read back, then len bytes of second after it, the whole range read
 * back through the same open and again through a new one. Upstream reads
 * back all of direct_io but only the first 128 KiB of async_direct_io.
 */
static void g130_direct_file(struct kunit *test, const char *path, u8 first,
			     u8 second, size_t len, size_t readback, u8 *buf)
{
	struct file *f;
	int pass;

	f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT);
	g130_fill(test, f, true, first, 0, 65536, buf, "the first fill");
	g130_expect(test, f, true, 0, 65536, G130_SEGS({ 65536, first }), buf,
		    "the first range");
	g130_fill(test, f, true, second, 65536, len, buf, "the second fill");

	for (pass = 0; pass < 2; pass++) {
		if (pass) {
			filp_close(f, NULL);
			f = g130_open(test, path, O_RDWR | O_DIRECT);
		}
		g130_expect(test, f, true, 0, readback,
			    G130_SEGS({ 65536, first }, { readback, second }),
			    buf, pass ? "the readback, reopened"
				      : "the readback");
	}
	filp_close(f, NULL);
}

static void direct_reads_and_writes_over_a_larger_range(struct kunit *test)
{
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	g130_direct_file(test, G130_ROOT "/direct_io", 0x78, 0x46, 6553600,
			 65536 + 6553600, buf);
	g130_direct_file(test, G130_ROOT "/async_direct_io", 0x61, 0x62,
			 131072, 131072, buf);
}

static void the_block_edge_case(struct kunit *test)
{
	const char *path = G130_ROOT "/fsb_edge_test";
	struct file *f;
	loff_t pos;
	u8 two[2] = { 0x61, 0x61 };
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 131072), 0);
	g130_fill(test, f, false, 0x5f, 0, 131072, buf, "the 0x5f fill");
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 131072), 0);
	pos = 65535;
	KUNIT_ASSERT_EQ(test, kernel_write(f, two, 2, &pos), 2L);

	g130_expect(test, f, false, 0, 131072,
		    G130_SEGS({ 65535, 0x00 }, { 65537, 0x61 },
			      { 131072, 0x00 }),
		    buf, "the whole file");
	filp_close(f, NULL);
}

/* xfs_io -f -t -c "pread -v 0 100": O_TRUNC, so it must read nothing */
static void g130_expect_truncated_on_open(struct kunit *test, const char *path,
					  u8 *buf, const char *what)
{
	struct file *f = g130_open(test, path, O_RDWR | O_CREAT | O_TRUNC);
	loff_t pos = 0;
	ssize_t r = kernel_read(f, buf, 100, &pos);

	filp_close(f, NULL);
	KUNIT_EXPECT_EQ_MSG(test, r, 0L, "%s: %s read %zd bytes", path, what,
			    r);
}

static void o_trunc_empties_an_existing_file(struct kunit *test)
{
	static const char msg[] = "Test\n";
	char path[64];
	u8 buf[100];
	int n;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);

	for (n = 0; n < 5; n++) {
		snprintf(path, sizeof(path), G130_ROOT "/%d", n);

		g130_expect_truncated_on_open(test, path, buf, "a new file");

		/* echo "Test" > $n */
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(path, msg, sizeof(msg) - 1),
				0);
		g130_expect_truncated_on_open(test, path, buf,
					      "a file holding Test");

		/* cat $n */
		KUNIT_EXPECT_EQ_MSG(test, xfs_read_range(path, buf, 100, 0),
				    0L, "%s is not empty after O_TRUNC", path);
	}
}

static void o_append_ignores_the_offset(struct kunit *test)
{
	static const char msg[] = "append to me\n";
	static const char want[] = "append to me\naaaaaaaaaa";
	const char *path = G130_ROOT "/append";
	struct file *f;
	loff_t pos = 0;
	u8 buf[24];
	ssize_t r;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(path, msg, sizeof(msg) - 1), 0);

	/* pwrite at offset 0 on an O_APPEND file still lands at the end */
	f = g130_open(test, path, O_RDWR | O_APPEND);
	memset(buf, 0x61, 10);
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 10, &pos), 10L);

	pos = 0;
	r = kernel_read(f, buf, sizeof(buf), &pos);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)(sizeof(want) - 1),
			    "read %zd of 24 bytes", r);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, want, sizeof(want) - 1), 0,
			    "the file is not the text followed by ten 'a's");
}

/* 16 one-byte writes of 'a'..'p' from base, read back four at a time */
static void g130_small_writes(struct kunit *test, struct file *f, loff_t base)
{
	u8 got[4];
	int i, j;

	for (i = 0; i < 16; i++) {
		loff_t pos = base + i;
		u8 c = 0x61 + i;

		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, &c, 1, &pos), 1L,
				    "writing byte %lld failed", base + i);
	}
	for (i = 0; i < 16; i += 4) {
		loff_t pos = base + i;

		KUNIT_ASSERT_EQ_MSG(test, kernel_read(f, got, 4, &pos), 4L,
				    "reading 4 bytes at %lld failed", base + i);
		for (j = 0; j < 4; j++)
			KUNIT_EXPECT_EQ_MSG(test, got[j], (u8)(0x61 + i + j),
					    "byte %lld is %02x, expected %02x",
					    base + i + j, got[j], 0x61 + i + j);
	}
}

static void g130_small_vector(struct kunit *test, int flags)
{
	static const char msg[] = "abcdefghijklmnopqrstuvwxyz\n";
	const char *path = G130_ROOT "/small_vector_async";
	struct file *f;
	loff_t pos;
	u8 got[13];
	int i;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);

	/* echo "abcdefghijklmnopqrstuvwxyz" > small_vector_async */
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(path, msg, sizeof(msg) - 1), 0);

	f = g130_open(test, path, O_RDWR | O_CREAT | flags);

	/* one byte at a time for 13 bytes, then the next 13 at once */
	for (i = 0; i < 13; i++) {
		pos = i;
		KUNIT_ASSERT_EQ_MSG(test, kernel_read(f, got, 1, &pos), 1L,
				    "reading byte %d failed", i);
		KUNIT_EXPECT_EQ_MSG(test, got[0], (u8)msg[i],
				    "byte %d is %02x, expected %02x", i, got[0],
				    msg[i]);
	}
	pos = 13;
	KUNIT_ASSERT_EQ(test, kernel_read(f, got, 13, &pos), 13L);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, msg + 13, 13), 0,
			    "bytes 13..25 are not nopqrstuvwxyz");

	/* either side of the page boundary, then far past EOF */
	g130_small_writes(test, f, 4090);
	g130_small_writes(test, f, G130_FAR);
	filp_close(f, NULL);
}

static void small_vector_reads_and_writes(struct kunit *test)
{
	g130_small_vector(test, 0);
}

static void small_vector_reads_and_writes_with_o_sync(struct kunit *test)
{
	g130_small_vector(test, O_SYNC);
}

static int g130_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g130_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g130_cases[] = {
	KUNIT_CASE(end_of_file_zeroing_with_direct_io),
	KUNIT_CASE(reading_a_hole_made_by_truncate),
	KUNIT_CASE(buffered_and_direct_writes_stay_coherent),
	KUNIT_CASE_SLOW(direct_reads_and_writes_over_a_larger_range),
	KUNIT_CASE(the_block_edge_case),
	KUNIT_CASE(o_trunc_empties_an_existing_file),
	KUNIT_CASE(o_append_ignores_the_offset),
	KUNIT_CASE(small_vector_reads_and_writes),
	KUNIT_CASE(small_vector_reads_and_writes_with_o_sync),
	{}
};

static struct kunit_suite g130_suite = {
	.name		= "xfstests/generic/130",
	.suite_init	= g130_suite_init,
	.suite_exit	= g130_suite_exit,
	.test_cases	= g130_cases,
};

kunit_test_suites(&g130_suite);

MODULE_DESCRIPTION("xfstests generic/130 over a loopback NFS mount");
MODULE_LICENSE("GPL");
