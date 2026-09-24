// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/285 over a loopback NFS mount: SEEK_DATA/SEEK_HOLE
 * sanity.
 *
 * Upstream runs src/seek_sanity_test with its default range, sub-tests 01
 * to 12 (later sub-tests belong to other cases: 18 to generic/448, 19-20
 * to generic/490, 22 to generic/706), each on a fresh file:
 *
 *	01 an empty file		02 a tiny full file
 *	03 a larger full file		04 hole at the start, data at the end
 *	05 data at the start, hole at the end
 *	06 hole, data, hole, data
 *	07-09 unwritten extents with dirty, writeback, and both kinds of
 *	    pages in the cache
 *	10-12 huge files: 8 GiB, alloc_size << 31 and << 32 bytes plus
 *	    1 MiB, for offset and block-count overflows
 *
 * test_basic_support() comes first: it finds the allocation unit by
 * probing where SEEK_DATA lands after a one-byte write, and checks whether
 * the filesystem only has the "default behaviour" (SEEK_HOLE always at
 * EOF), supports unwritten extents (a fallocated range is not data) and
 * punch hole. For NFSv4.2 _run_seek_sanity_test passes -f, so default
 * behaviour is a failure rather than something to accept.
 *
 * Every lseek is checked as do_lseek() checks it: the expected offset, or
 * for SEEK_HOLE the end of the file (an implementation may report EOF as
 * the next hole), and ENXIO where the expected answer is -1.
 *
 * Over NFSv4.2 each SEEK_HOLE/SEEK_DATA is a SEEK RPC answered from the
 * tmpfs export's own page map; the client flushes dirty pages first, so
 * the dirty-page sub-tests exercise that flush.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G285_ROOT	XFS_MNT "/g285"
#define G285_BASE	G285_ROOT "/seek_sanity_testfile"

struct g285_ctx {
	struct kunit	*test;
	loff_t		alloc_size;
	bool		default_behavior;
	bool		unwritten_extents;
	bool		punch_hole;
};

static void g285_path(char *buf, size_t len, int num)
{
	if (num)
		snprintf(buf, len, G285_BASE "%02d", num);
	else
		snprintf(buf, len, G285_BASE);
}

static void g285_remove_tree(void *unused)
{
	char path[96];
	int i;

	for (i = 0; i <= 12; i++) {
		g285_path(path, sizeof(path), i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G285_ROOT);
}

/* do_create(): O_RDWR | O_CREAT | O_TRUNC, 0644 */
static struct file *g285_create(struct kunit *test, int num)
{
	char path[96];
	struct file *f;

	g285_path(path, sizeof(path), num);
	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create %s: %ld", path,
			       PTR_ERR(f));
	return f;
}

/* do_pwrite(): 0, or the error (EFBIG is the caller's to judge) */
static int g285_pwrite(struct file *f, const void *buf, size_t count,
		       loff_t off)
{
	ssize_t n = kernel_write(f, buf, count, &off);

	if (n < 0)
		return n;
	return n == count ? 0 : -EIO;
}

static void g285_lseek(struct g285_ctx *c, struct file *f, int testnum,
		       int subtest, loff_t filsz, int origin, loff_t set,
		       loff_t exp)
{
	loff_t pos, exp2 = exp;

	if (origin == SEEK_HOLE && exp2 != -1)
		exp2 = filsz;
	if (origin == SEEK_DATA && c->default_behavior && set < filsz)
		exp2 = set;

	pos = vfs_llseek(f, set, origin);
	if (pos < 0 && exp == -1)
		KUNIT_EXPECT_EQ_MSG(c->test, pos, (loff_t)-ENXIO,
				    "%02d.%02d %s expected -1 with errno %d, got %lld",
				    testnum, subtest,
				    origin == SEEK_HOLE ? "SEEK_HOLE" : "SEEK_DATA",
				    -ENXIO, pos);
	else
		KUNIT_EXPECT_TRUE_MSG(c->test, pos == exp || pos == exp2,
				      "%02d.%02d %s expected %lld or %lld, got %lld",
				      testnum, subtest,
				      origin == SEEK_HOLE ? "SEEK_HOLE" : "SEEK_DATA",
				      exp, exp2, pos);
}

/* get_io_sizes(): where does SEEK_DATA land after a one-byte write? */
static void g285_get_io_sizes(struct g285_ctx *c, struct file *f)
{
	struct kstat st;
	loff_t pos = 0, offset = 1, shift;

	KUNIT_ASSERT_EQ(c->test, vfs_getattr(&f->f_path, &st,
					     STATX_BASIC_STATS,
					     AT_STATX_SYNC_AS_STAT), 0);
	c->alloc_size = st.blksize;
	while (pos == 0 && offset < c->alloc_size) {
		offset <<= 1;
		KUNIT_ASSERT_EQ(c->test, xfs_ftruncate(f, 0), 0);
		KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, "a", 1, offset), 0);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE_MSG(c->test, pos, 0LL,
				    "Kernel does not support llseek(2) extension SEEK_DATA");
	}
	shift = offset >> 2;
	while (shift && offset < c->alloc_size) {
		KUNIT_ASSERT_EQ(c->test, xfs_ftruncate(f, 0), 0);
		KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, "a", 1, offset), 0);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE(c->test, pos, 0LL);
		offset += pos ? -shift : shift;
		shift >>= 1;
	}
	if (!shift)
		offset += pos ? 0 : 1;
	c->alloc_size = offset;
	kunit_info(c->test, "Allocation size: %lld\n", c->alloc_size);
}

/* test_basic_support() */
static void g285_basic_support(struct g285_ctx *c)
{
	struct kunit *test = c->test;
	struct file *f = g285_create(test, 0);
	loff_t bufsz, filsz, pos;
	u8 *buf;

	g285_get_io_sizes(c, f);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	bufsz = c->alloc_size * 2;
	filsz = bufsz * 2;
	buf = kunit_kmalloc(test, bufsz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'a', bufsz);

	/* two allocated blocks followed by a hole */
	KUNIT_ASSERT_EQ(test, g285_pwrite(f, buf, bufsz, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, filsz), 0);
	KUNIT_ASSERT_GE(test, vfs_llseek(f, 0, SEEK_DATA), 0LL);
	pos = vfs_llseek(f, 0, SEEK_HOLE);
	KUNIT_ASSERT_GE(test, pos, 0LL);
	c->default_behavior = pos == filsz;
	/* -f: NFSv4.2 is on _fstyp_has_non_default_seek_data_hole's list */
	KUNIT_ASSERT_FALSE_MSG(test, c->default_behavior,
			       "Default behavior is not allowed. Aborting.");

	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, c->alloc_size * 2), 0,
			    "Failed to preallocate space");
	c->unwritten_extents = vfs_llseek(f, 0, SEEK_DATA) != 0;
	c->punch_hole = !vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
				       FALLOC_FL_KEEP_SIZE, 0, c->alloc_size);
	kunit_info(test, "unwritten extents: %d, punch hole: %d\n",
		   c->unwritten_extents, c->punch_hole);
	kunit_kfree(test, buf);
	filp_close(f, NULL);
}

/* test empty file */
static void test01(struct g285_ctx *c, struct file *f, int t)
{
	g285_lseek(c, f, t, 1, 0, SEEK_DATA, 0, -1);
	g285_lseek(c, f, t, 2, 0, SEEK_HOLE, 0, -1);
	g285_lseek(c, f, t, 3, 0, SEEK_HOLE, 1, -1);
}

/* test tiny full file */
static void test02(struct g285_ctx *c, struct file *f, int t)
{
	const char buf[] = "ABCDEFGH";
	loff_t bufsz = strlen(buf), filsz = bufsz;

	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, 0), 0);
	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, filsz);
	g285_lseek(c, f, t, 2, filsz, SEEK_DATA, 0, 0);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 1, 1);
	g285_lseek(c, f, t, 4, filsz, SEEK_HOLE, bufsz - 1, filsz);
	g285_lseek(c, f, t, 5, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);
	g285_lseek(c, f, t, 6, filsz, SEEK_HOLE, bufsz, -1);
	g285_lseek(c, f, t, 7, filsz, SEEK_DATA, bufsz, -1);
	g285_lseek(c, f, t, 8, filsz, SEEK_HOLE, bufsz + 1, -1);
	g285_lseek(c, f, t, 9, filsz, SEEK_DATA, bufsz + 1, -1);
}

/* test a larger full file */
static void test03(struct g285_ctx *c, struct file *f, int t)
{
	loff_t bufsz = c->alloc_size * 2 + 100, filsz = bufsz;
	u8 *buf = kunit_kmalloc(c->test, bufsz, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(c->test, buf);
	memset(buf, 'a', bufsz);
	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, 0), 0);
	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, bufsz);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, bufsz);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, 0);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, 1);
	g285_lseek(c, f, t, 5, filsz, SEEK_HOLE, bufsz - 1, bufsz);
	g285_lseek(c, f, t, 6, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);
	g285_lseek(c, f, t, 7, filsz, SEEK_HOLE, bufsz, -1);
	g285_lseek(c, f, t, 8, filsz, SEEK_DATA, bufsz, -1);
	g285_lseek(c, f, t, 9, filsz, SEEK_HOLE, bufsz + 1, -1);
	g285_lseek(c, f, t, 10, filsz, SEEK_DATA, bufsz + 1, -1);
}

/* test hole begin and data end */
static void test04(struct g285_ctx *c, struct file *f, int t)
{
	const char buf[] = "ABCDEFGH";
	loff_t bufsz = strlen(buf), holsz = c->alloc_size * 2;
	loff_t filsz = holsz + bufsz;

	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, holsz), 0);
	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, 0);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, 1);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, holsz);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, holsz);
	g285_lseek(c, f, t, 5, filsz, SEEK_HOLE, holsz - 1, holsz - 1);
	g285_lseek(c, f, t, 6, filsz, SEEK_DATA, holsz - 1, holsz);
	g285_lseek(c, f, t, 7, filsz, SEEK_HOLE, holsz, filsz);
	g285_lseek(c, f, t, 8, filsz, SEEK_DATA, holsz, holsz);
	g285_lseek(c, f, t, 9, filsz, SEEK_HOLE, holsz + 1, filsz);
	g285_lseek(c, f, t, 10, filsz, SEEK_DATA, holsz + 1, holsz + 1);
	g285_lseek(c, f, t, 11, filsz, SEEK_HOLE, filsz - 1, filsz);
	g285_lseek(c, f, t, 12, filsz, SEEK_DATA, filsz - 1, filsz - 1);
	g285_lseek(c, f, t, 13, filsz, SEEK_HOLE, filsz, -1);
	g285_lseek(c, f, t, 14, filsz, SEEK_DATA, filsz, -1);
	g285_lseek(c, f, t, 15, filsz, SEEK_HOLE, filsz + 1, -1);
	g285_lseek(c, f, t, 16, filsz, SEEK_DATA, filsz + 1, -1);
}

/* test file with data at the beginning and a hole at the end */
static void test05(struct g285_ctx *c, struct file *f, int t)
{
	loff_t bufsz = c->alloc_size, filsz = bufsz * 4;
	u8 *buf = kunit_kmalloc(c->test, bufsz, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(c->test, buf);
	memset(buf, 'a', bufsz);
	KUNIT_ASSERT_EQ(c->test, xfs_ftruncate(f, filsz), 0);
	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, 0), 0);
	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, bufsz);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, bufsz);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, 0);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, 1);
	g285_lseek(c, f, t, 5, filsz, SEEK_HOLE, bufsz - 1, bufsz);
	g285_lseek(c, f, t, 6, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);
	g285_lseek(c, f, t, 7, filsz, SEEK_HOLE, bufsz, bufsz);
	g285_lseek(c, f, t, 8, filsz, SEEK_DATA, bufsz, -1);
	g285_lseek(c, f, t, 9, filsz, SEEK_HOLE, bufsz + 1, bufsz + 1);
	g285_lseek(c, f, t, 10, filsz, SEEK_DATA, bufsz + 1, -1);
	g285_lseek(c, f, t, 11, filsz, SEEK_HOLE, filsz - 1, filsz - 1);
	g285_lseek(c, f, t, 12, filsz, SEEK_DATA, filsz - 1, -1);
	g285_lseek(c, f, t, 13, filsz, SEEK_HOLE, filsz, -1);
	g285_lseek(c, f, t, 14, filsz, SEEK_DATA, filsz, -1);
	g285_lseek(c, f, t, 15, filsz, SEEK_HOLE, filsz + 1, -1);
	g285_lseek(c, f, t, 16, filsz, SEEK_DATA, filsz + 1, -1);
}

/* test hole data hole data */
static void test06(struct g285_ctx *c, struct file *f, int t)
{
	loff_t bufsz = c->alloc_size, filsz = bufsz * 4, off;
	u8 *buf = kunit_kmalloc(c->test, bufsz, GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(c->test, buf);
	memset(buf, 'a', bufsz);
	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, bufsz), 0);
	g285_pwrite(f, buf, bufsz, bufsz * 3);	/* unchecked upstream */

	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, 0);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, 1);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, bufsz);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, bufsz);

	off = bufsz;
	g285_lseek(c, f, t, 5, filsz, SEEK_HOLE, off - 1, off - 1);
	g285_lseek(c, f, t, 6, filsz, SEEK_DATA, off - 1, off);
	g285_lseek(c, f, t, 7, filsz, SEEK_HOLE, off, bufsz * 2);
	g285_lseek(c, f, t, 8, filsz, SEEK_DATA, off, off);
	g285_lseek(c, f, t, 9, filsz, SEEK_HOLE, off + 1, bufsz * 2);
	g285_lseek(c, f, t, 10, filsz, SEEK_DATA, off + 1, off + 1);

	off = bufsz * 2;
	g285_lseek(c, f, t, 11, filsz, SEEK_HOLE, off - 1, off);
	g285_lseek(c, f, t, 12, filsz, SEEK_DATA, off - 1, off - 1);
	g285_lseek(c, f, t, 13, filsz, SEEK_HOLE, off, off);
	g285_lseek(c, f, t, 14, filsz, SEEK_DATA, off, bufsz * 3);
	g285_lseek(c, f, t, 15, filsz, SEEK_HOLE, off + 1, off + 1);
	g285_lseek(c, f, t, 16, filsz, SEEK_DATA, off + 1, bufsz * 3);

	off = bufsz * 3;
	g285_lseek(c, f, t, 17, filsz, SEEK_HOLE, off - 1, off - 1);
	g285_lseek(c, f, t, 18, filsz, SEEK_DATA, off - 1, off);
	g285_lseek(c, f, t, 19, filsz, SEEK_HOLE, off, filsz);
	g285_lseek(c, f, t, 20, filsz, SEEK_DATA, off, off);
	g285_lseek(c, f, t, 21, filsz, SEEK_HOLE, off + 1, filsz);
	g285_lseek(c, f, t, 22, filsz, SEEK_DATA, off + 1, off + 1);

	off = filsz;
	g285_lseek(c, f, t, 23, filsz, SEEK_HOLE, off - 1, filsz);
	g285_lseek(c, f, t, 24, filsz, SEEK_DATA, off - 1, filsz - 1);
	g285_lseek(c, f, t, 25, filsz, SEEK_HOLE, off, -1);
	g285_lseek(c, f, t, 26, filsz, SEEK_DATA, off, -1);
	g285_lseek(c, f, t, 27, filsz, SEEK_HOLE, off + 1, -1);
	g285_lseek(c, f, t, 28, filsz, SEEK_DATA, off + 1, -1);
}

/*
 * 07-09: unwritten extents -- a fallocated file with one written block
 * (07: still dirty), synced out (08: under or after writeback), or one
 * block of each (09).
 */
static void g285_unwritten(struct g285_ctx *c, struct file *f, int t,
			   int blocks, bool second, loff_t sync_from)
{
	loff_t bufsz = c->alloc_size, filsz = bufsz * blocks + bufsz;
	u8 *buf;

	if (!c->unwritten_extents) {
		kunit_info(c->test, "%02d: skipped, no unwritten extents\n", t);
		return;
	}
	buf = kunit_kmalloc(c->test, bufsz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(c->test, buf);
	memset(buf, 'a', bufsz);

	KUNIT_ASSERT_EQ(c->test, vfs_fallocate(f, 0, 0, filsz), 0);
	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, bufsz * 10), 0);
	if (second)
		KUNIT_ASSERT_EQ(c->test,
				g285_pwrite(f, buf, bufsz, bufsz * 100), 0);
	if (sync_from >= 0)
		KUNIT_ASSERT_EQ(c->test,
				sync_file_range(f, sync_from, 0,
						SYNC_FILE_RANGE_WRITE), 0);

	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, 0);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, 1);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, bufsz * 10);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, bufsz * 10);
}

/* test file with unwritten extents, only have dirty pages */
static void test07(struct g285_ctx *c, struct file *f, int t)
{
	g285_unwritten(c, f, t, 10, false, -1);
}

/* test file with unwritten extent, only have writeback page */
static void test08(struct g285_ctx *c, struct file *f, int t)
{
	g285_unwritten(c, f, t, 10, false, 0);
}

/* test file with unwritten extents, have both dirty and writeback pages */
static void test09(struct g285_ctx *c, struct file *f, int t)
{
	g285_unwritten(c, f, t, 100, true, c->alloc_size * 100);
}

/* |- DATA -|- HUGE HOLE -|- DATA -| */
static void g285_huge_file(struct g285_ctx *c, struct file *f, int t,
			   loff_t filsz)
{
	loff_t bufsz = c->alloc_size * 16, off = filsz - bufsz;
	u8 *buf = kunit_kmalloc(c->test, bufsz, GFP_KERNEL);
	int err;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(c->test, buf);
	memset(buf, 'a', bufsz);
	KUNIT_ASSERT_EQ(c->test, g285_pwrite(f, buf, bufsz, 0), 0);
	err = g285_pwrite(f, buf, bufsz, off);
	if (err == -EFBIG) {
		kunit_info(c->test, "%02d: skipped, fs doesn't support so large files\n",
			   t);
		return;
	}
	KUNIT_ASSERT_EQ(c->test, err, 0);

	g285_lseek(c, f, t, 1, filsz, SEEK_HOLE, 0, bufsz);
	g285_lseek(c, f, t, 2, filsz, SEEK_HOLE, 1, bufsz);
	g285_lseek(c, f, t, 3, filsz, SEEK_DATA, 0, 0);
	g285_lseek(c, f, t, 4, filsz, SEEK_DATA, 1, 1);
	g285_lseek(c, f, t, 5, filsz, SEEK_HOLE, off, off + bufsz);
	g285_lseek(c, f, t, 6, filsz, SEEK_DATA, off, off);
	g285_lseek(c, f, t, 7, filsz, SEEK_DATA, off + 1, off + 1);
	g285_lseek(c, f, t, 8, filsz, SEEK_DATA, off - bufsz, off);
}

/* Test an 8G file to check for offset overflows at 1 << 32 */
static void test10(struct g285_ctx *c, struct file *f, int t)
{
	g285_huge_file(c, f, t, 8LL << 30);
}

/* block counts in signed 32-bit types */
static void test11(struct g285_ctx *c, struct file *f, int t)
{
	g285_huge_file(c, f, t, (c->alloc_size << 31) + (1 << 20));
}

/* block counts in 32-bit types */
static void test12(struct g285_ctx *c, struct file *f, int t)
{
	g285_huge_file(c, f, t, (c->alloc_size << 32) + (1 << 20));
}

static void (*const g285_tests[])(struct g285_ctx *, struct file *, int) = {
	test01, test02, test03, test04, test05, test06,
	test07, test08, test09, test10, test11, test12,
};

static void seek_sanity_tests_01_to_12(struct kunit *test)
{
	struct g285_ctx c = { .test = test };
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G285_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g285_remove_tree, NULL),
			0);

	g285_basic_support(&c);
	for (i = 0; i < ARRAY_SIZE(g285_tests); i++) {
		struct file *f = g285_create(test, i + 1);

		g285_tests[i](&c, f, i + 1);
		filp_close(f, NULL);
	}
}

static int g285_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g285_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g285_cases[] = {
	KUNIT_CASE_SLOW(seek_sanity_tests_01_to_12),
	{}
};

static struct kunit_suite g285_suite = {
	.name		= "xfstests/generic/285",
	.suite_init	= g285_suite_init,
	.suite_exit	= g285_suite_exit,
	.test_cases	= g285_cases,
};

kunit_test_suites(&g285_suite);

MODULE_DESCRIPTION("xfstests generic/285 over a loopback NFS mount");
MODULE_LICENSE("GPL");
