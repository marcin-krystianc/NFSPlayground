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
 *	direct reads and writes over a larger range
 *	the "FSB edge" case: truncate away, re-extend, write across 64k
 *	O_TRUNC on an existing file
 *	O_APPEND ignoring the offset it is given
 *	single-byte reads and writes either side of a page boundary
 *
 * Over NFS the interesting ones are the mixed cases: a direct write has
 * to invalidate whatever the client had cached for its range, and a
 * buffered read afterwards has to fetch it from the server rather than
 * serve the pre-write page. The hole cases check that the client
 * zero-fills rather than returning whatever the page held.
 *
 * Deviations: the 6.25 MiB range in the direct read/write scenario is
 * 640 KiB here (the export is a 64 MiB tmpfs), and the golden hexdumps
 * are replaced by byte-exact expectations. The last scenario keeps
 * upstream's offsets but writes its bytes in a loop.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G130_ROOT	XFS_MNT "/g130"
#define G130_MAX	(640 * 1024)

static const char * const g130_files[] = {
	"eof-zeroing_direct", "blackhole", "buff_direct_coherency",
	"direct_io", "fsb_edge_test", "trunc0", "append", "small_vector",
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

/* write len bytes of val at off, through f */
static void g130_fill(struct kunit *test, struct file *f, bool direct, u8 val,
		      loff_t off, size_t len, u8 *scratch, const char *what)
{
	loff_t pos = off;
	size_t done = 0;

	while (done < len) {
		size_t n = min(len - done, (size_t)G130_MAX);
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

/* every byte of [off, off+len) must equal val */
static void g130_expect(struct kunit *test, const char *path, loff_t off,
			size_t len, u8 val, u8 *scratch, const char *what)
{
	size_t done = 0;

	while (done < len) {
		size_t n = min(len - done, (size_t)G130_MAX);
		ssize_t r = xfs_read_range(path, scratch, n, off + done);
		int i;

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "%s: read at %lld returned %zd", what,
				    off + done, r);
		for (i = 0; i < n; i++)
			if (scratch[i] != val) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   what, off + done + i, scratch[i],
					   val);
				return;
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
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	g130_fill(test, f, true, 0x63, 0, 65536, buf, "the 0x63 fill");
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 1), 0);
	g130_fill(test, f, true, 0x41, 65536, 65536, buf, "the 0x41 fill");
	filp_close(f, NULL);

	g130_expect(test, path, 0, 1, 0x63, buf, "the surviving byte");
	g130_expect(test, path, 1, 65535, 0x00, buf, "the truncated range");
	g130_expect(test, path, 65536, 65536, 0x41, buf, "the second write");
}

static void reading_a_hole_made_by_truncate(struct kunit *test)
{
	const char *path = G130_ROOT "/blackhole";
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_truncate(path, 8192), 0);

	g130_expect(test, path, 5000, 3000, 0x00, buf, "the hole");
}

static void buffered_and_direct_writes_stay_coherent(struct kunit *test)
{
	const char *path = G130_ROOT "/buff_direct_coherency";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	g130_fill(test, f, false, 0x41, 8000, 1000, buf, "buffered 0x41");
	g130_fill(test, f, false, 0x57, 4000, 1000, buf, "buffered 0x57");
	filp_close(f, NULL);

	f = filp_open(path, O_RDWR | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct open: %ld",
			       PTR_ERR(f));
	g130_fill(test, f, true, 0x78, 20480, 4096, buf, "direct 0x78");
	g130_fill(test, f, true, 0x79, 4096, 4096, buf, "direct 0x79");
	filp_close(f, NULL);

	/* the direct writes came last, so they own their ranges */
	g130_expect(test, path, 0, 4000, 0x00, buf, "the leading hole");
	g130_expect(test, path, 4000, 96, 0x57, buf, "what 0x57 kept");
	g130_expect(test, path, 4096, 4096, 0x79, buf, "the 0x79 range");
	g130_expect(test, path, 8192, 808, 0x41, buf, "what 0x41 kept");
	g130_expect(test, path, 20480, 4096, 0x78, buf, "the 0x78 range");
}

static void direct_reads_and_writes_over_a_larger_range(struct kunit *test)
{
	const char *path = G130_ROOT "/direct_io";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	g130_fill(test, f, true, 0x78, 0, 65536, buf, "the 0x78 fill");
	g130_fill(test, f, true, 0x46, 65536, G130_MAX, buf, "the 0x46 fill");
	filp_close(f, NULL);

	g130_expect(test, path, 0, 65536, 0x78, buf, "the first range");
	g130_expect(test, path, 65536, G130_MAX, 0x46, buf,
		    "the second range");
}

static void the_block_edge_case(struct kunit *test)
{
	const char *path = G130_ROOT "/fsb_edge_test";
	struct file *f;
	loff_t pos;
	u8 two[2] = { 0x61, 0x61 };
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 131072), 0);
	g130_fill(test, f, false, 0x5f, 0, 131072, buf, "the 0x5f fill");
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 131072), 0);
	pos = 65535;
	KUNIT_ASSERT_EQ(test, kernel_write(f, two, 2, &pos), 2L);
	filp_close(f, NULL);

	g130_expect(test, path, 0, 65535, 0x00, buf, "before the edge");
	g130_expect(test, path, 65535, 2, 0x61, buf, "across the edge");
	g130_expect(test, path, 65537, 131072 - 65537, 0x00, buf,
		    "after the edge");
}

static void o_trunc_empties_an_existing_file(struct kunit *test)
{
	static const char msg[] = "Test\n";
	const char *path = G130_ROOT "/trunc0";
	struct kstat st;
	char got[8];
	int n;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);

	for (n = 0; n < 5; n++) {
		struct file *f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC,
					   0644);
		loff_t pos = 0;

		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "round %d: open: %ld",
				       n, PTR_ERR(f));
		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
				    "round %d: O_TRUNC left %lld bytes", n,
				    st.size);
		KUNIT_ASSERT_EQ(test,
				kernel_write(f, msg, sizeof(msg) - 1, &pos),
				(ssize_t)(sizeof(msg) - 1));
		filp_close(f, NULL);

		KUNIT_ASSERT_EQ(test,
				xfs_read_range(path, got, sizeof(msg) - 1, 0),
				(ssize_t)(sizeof(msg) - 1));
		KUNIT_EXPECT_EQ_MSG(test, memcmp(got, msg, sizeof(msg) - 1), 0,
				    "round %d: the file does not hold Test", n);
	}
}

static void o_append_ignores_the_offset(struct kunit *test)
{
	static const char msg[] = "append to me\n";
	const char *path = G130_ROOT "/append";
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(path, msg, sizeof(msg) - 1), 0);

	/* pwrite at offset 0 on an O_APPEND file still lands at the end */
	f = filp_open(path, O_WRONLY | O_APPEND, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	memset(buf, 0x61, 10);
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 10, &pos), 10L);
	filp_close(f, NULL);

	g130_expect(test, path, 0, 1, msg[0], buf, "the original text");
	g130_expect(test, path, sizeof(msg) - 1, 10, 0x61, buf,
		    "the appended bytes");
}

static void single_byte_reads_and_writes_around_a_page(struct kunit *test)
{
	static const char msg[] = "abcdefghijklmnopqrstuvwxyz\n";
	const char *path = G130_ROOT "/small_vector";
	struct file *f;
	u8 *buf;
	int i;

	KUNIT_ASSERT_EQ(test, g130_setup(test), 0);
	buf = kunit_kmalloc(test, G130_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(path, msg, sizeof(msg) - 1), 0);

	/* one byte at a time, from the front */
	for (i = 0; i < 13; i++) {
		u8 c;

		KUNIT_ASSERT_EQ_MSG(test, xfs_read_range(path, &c, 1, i), 1L,
				    "reading byte %d failed", i);
		KUNIT_EXPECT_EQ_MSG(test, c, (u8)msg[i],
				    "byte %d is %02x, expected %02x", i, c,
				    msg[i]);
	}

	/* and single-byte writes either side of the page boundary */
	f = filp_open(path, O_RDWR, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	for (i = 0; i < 12; i++) {
		loff_t pos = 4090 + i;
		u8 c = 0x61 + i;

		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, &c, 1, &pos), 1L,
				    "writing byte %lld failed", pos);
	}
	filp_close(f, NULL);

	for (i = 0; i < 12; i++) {
		u8 c;

		KUNIT_ASSERT_EQ(test, xfs_read_range(path, &c, 1, 4090 + i),
				1L);
		KUNIT_EXPECT_EQ_MSG(test, c, (u8)(0x61 + i),
				    "byte %d is %02x, expected %02x", 4090 + i,
				    c, 0x61 + i);
	}
	/* the gap between the text and 4090 is a hole */
	g130_expect(test, path, sizeof(msg) - 1, 4090 - (sizeof(msg) - 1),
		    0x00, buf, "the hole before the single-byte writes");
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
	KUNIT_CASE(single_byte_reads_and_writes_around_a_page),
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
