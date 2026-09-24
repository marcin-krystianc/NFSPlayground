// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/286 over a loopback NFS mount: SEEK_DATA/SEEK_HOLE copy.
 *
 * Upstream builds four sparse source files with xfs_io and copies each
 * with src/seek_copy_test, which walks the source with SEEK_DATA and
 * SEEK_HOLE, copies every data extent at its own offset in 4096-byte
 * reads, and ftruncates the copy to the source's size. Each test then
 * checks that the sizes match and that cmp(1) finds the files identical.
 *
 *	test01: truncate 100m, pwrite 1m at every 5 MiB from 0 to 100 MiB
 *	test02: truncate 200m, falloc 3m + pwrite 1m at 6 MiB + every 10 MiB
 *	test03: truncate 200m, falloc 10m at 10 MiB + every 10 MiB up to
 *		190 MiB, then pwrite 10m at 20 MiB + every 60 MiB
 *	test04: truncate 200m, falloc 5m at 60 MiB + every 30 MiB, then
 *		pwrite 2m at 60 MiB + every 90 MiB
 *
 * Over NFSv4.2 the walk is SEEK RPCs and falloc is ALLOCATE. On the tmpfs
 * export an ALLOCATE'd range that was never written may report as data or
 * as a hole; either way the copy must come out identical.
 *
 * The sources, the copy loop and the checks are upstream's, at upstream's
 * offsets and sizes. Deviations: pwrite fills with 0xcd, as a non-zero
 * byte, in 1 MiB calls; cmp is a chunked memcmp; the export is 512 MiB
 * instead of the fixture's 64 MiB default, because test03's source
 * allocates 190 MiB and its copy may hold as much again.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G286_ROOT	XFS_MNT "/g286"
#define G286_SRC	G286_ROOT "/seek_copy_testfile"
#define G286_DEST	G286_ROOT "/seek_copy_testfile.dest"
#define G286_EXPORT	"size=536870912,nr_inodes=32768"
#define G286_MIB	(1024 * 1024LL)
#define G286_CHUNK	(1024 * 1024)
#define G286_BUF_SIZE	4096		/* seek_copy_test's BUF_SIZE */

static void g286_remove_tree(void *unused)
{
	xfs_unlink(G286_SRC);
	xfs_unlink(G286_DEST);
	xfs_rmdir_settled(G286_ROOT);
}

static struct file *g286_create_src(struct kunit *test, loff_t size)
{
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G286_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g286_remove_tree, NULL),
			0);

	f = filp_open(G286_SRC, O_RDWR | O_CREAT | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, size), 0);
	return f;
}

static void g286_falloc(struct kunit *test, struct file *f, loff_t off,
			loff_t len)
{
	int err = vfs_fallocate(f, 0, off, len);

	KUNIT_ASSERT_EQ_MSG(test, err, 0, "falloc %lld %lld: %d", off, len,
			    err);
}

static void g286_pwrite(struct kunit *test, struct file *f, loff_t off,
			loff_t len)
{
	u8 *buf = kunit_kmalloc(test, G286_CHUNK, GFP_KERNEL);
	loff_t pos = off;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, G286_CHUNK);
	while (pos < off + len) {
		size_t n = min_t(loff_t, off + len - pos, G286_CHUNK);

		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, buf, n, &pos),
				    (ssize_t)n, "pwrite at %lld", pos);
	}
	kunit_kfree(test, buf);
}

/* seek_copy_test's do_extent_copy(), with its unsigned len arithmetic */
static int g286_extent_copy(struct file *src, struct file *dest, u8 *buf,
			    loff_t data_off, loff_t hole_off)
{
	u64 len = (u64)(hole_off - data_off);
	loff_t rpos = data_off, wpos = data_off;

	while (len > 0) {
		ssize_t nr_read = kernel_read(src, buf, G286_BUF_SIZE, &rpos);
		ssize_t nr_written;

		if (nr_read < 0)
			return nr_read;
		if (nr_read == 0)
			break;	/* "reached EOF" */
		nr_written = kernel_write(dest, buf, nr_read, &wpos);
		if (nr_written != nr_read)
			return nr_written < 0 ? nr_written : -EIO;
		len -= nr_read;
	}
	return 0;
}

/* seek_copy_test's main() and copy_extents() */
static void g286_seek_copy(struct kunit *test)
{
	struct file *src, *dest;
	struct kstat st;
	loff_t seek_start = 0, dest_pos = 0, data_pos, hole_pos;
	loff_t src_total_size;
	u8 *buf;
	int ret = 0;

	buf = kunit_kmalloc(test, G286_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	src = filp_open(G286_SRC, O_RDONLY | O_LARGEFILE, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "open src: %ld",
			       PTR_ERR(src));
	dest = filp_open(G286_DEST, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE,
			 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(dest), "create dest: %ld",
			       PTR_ERR(dest));
	KUNIT_ASSERT_EQ(test, xfs_kstat(G286_SRC, &st), 0);
	src_total_size = st.size;

	do {
		data_pos = vfs_llseek(src, seek_start, SEEK_DATA);
		if (data_pos < 0) {
			if (data_pos != -ENXIO)
				ret = data_pos;
			break;
		}
		hole_pos = vfs_llseek(src, data_pos, SEEK_HOLE);
		if (hole_pos < 0) {
			if (hole_pos != -ENXIO)
				ret = hole_pos;
			break;
		}
		ret = g286_extent_copy(src, dest, buf, data_pos, hole_pos);
		if (ret < 0)
			break;
		dest_pos += hole_pos - data_pos;
		seek_start = hole_pos;
	} while (seek_start < src_total_size);

	if (!ret && dest_pos < src_total_size)
		ret = xfs_ftruncate(dest, src_total_size);

	filp_close(dest, NULL);
	filp_close(src, NULL);
	KUNIT_ASSERT_EQ_MSG(test, ret, 0, "seek_copy_test failed: %d", ret);
}

/* the size check and cmp(1) */
static void g286_verify(struct kunit *test)
{
	struct kstat s, d;
	u8 *a, *b;
	loff_t off;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G286_SRC, &s), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G286_DEST, &d), 0);
	KUNIT_ASSERT_EQ_MSG(test, d.size, s.size,
			    "file size check failed: dest %lld, src %lld",
			    d.size, s.size);

	a = kunit_kmalloc(test, G286_CHUNK, GFP_KERNEL);
	b = kunit_kmalloc(test, G286_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	for (off = 0; off < s.size; off += G286_CHUNK) {
		size_t n = min_t(loff_t, s.size - off, G286_CHUNK);
		size_t i;

		KUNIT_ASSERT_EQ(test, xfs_read_range(G286_SRC, a, n, off),
				(ssize_t)n);
		KUNIT_ASSERT_EQ(test, xfs_read_range(G286_DEST, b, n, off),
				(ssize_t)n);
		if (memcmp(a, b, n)) {
			for (i = 0; a[i] == b[i]; i++)
				;
			KUNIT_FAIL(test,
				   "file bytes check failed: byte %lld is %02x in src, %02x in dest",
				   off + i, a[i], b[i]);
			return;
		}
	}
}

static void test01(struct kunit *test)
{
	struct file *f = g286_create_src(test, 100 * G286_MIB);
	int i;

	for (i = 0; i <= 100; i += 5)
		g286_pwrite(test, f, i * G286_MIB, G286_MIB);
	filp_close(f, NULL);

	g286_seek_copy(test);
	g286_verify(test);
}

static void test02(struct kunit *test)
{
	struct file *f = g286_create_src(test, 200 * G286_MIB);
	int i;

	for (i = 0; i <= 100; i += 10) {
		loff_t off = 6 * G286_MIB + i * G286_MIB;

		g286_falloc(test, f, off, 3 * G286_MIB);
		g286_pwrite(test, f, off, G286_MIB);
	}
	filp_close(f, NULL);

	g286_seek_copy(test);
	g286_verify(test);
}

static void test03(struct kunit *test)
{
	struct file *f = g286_create_src(test, 200 * G286_MIB);
	int i;

	for (i = 0; i <= 180; i += 10)
		g286_falloc(test, f, 10 * G286_MIB + i * G286_MIB,
			    10 * G286_MIB);
	for (i = 0; i <= 180; i += 60)
		g286_pwrite(test, f, 20 * G286_MIB + i * G286_MIB,
			    10 * G286_MIB);
	filp_close(f, NULL);

	g286_seek_copy(test);
	g286_verify(test);
}

static void test04(struct kunit *test)
{
	struct file *f = g286_create_src(test, 200 * G286_MIB);
	int i;

	for (i = 30; i <= 180; i += 30)
		g286_falloc(test, f, 30 * G286_MIB + i * G286_MIB,
			    5 * G286_MIB);
	for (i = 30; i <= 180; i += 90)
		g286_pwrite(test, f, 30 * G286_MIB + i * G286_MIB,
			    2 * G286_MIB);
	filp_close(f, NULL);

	g286_seek_copy(test);
	g286_verify(test);
}

static int g286_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G286_EXPORT);
	return xfstests_nfs_get();
}

static void g286_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g286_cases[] = {
	KUNIT_CASE_SLOW(test01),
	KUNIT_CASE_SLOW(test02),
	KUNIT_CASE_SLOW(test03),
	KUNIT_CASE_SLOW(test04),
	{}
};

static struct kunit_suite g286_suite = {
	.name		= "xfstests/generic/286",
	.suite_init	= g286_suite_init,
	.suite_exit	= g286_suite_exit,
	.test_cases	= g286_cases,
};

kunit_test_suites(&g286_suite);

MODULE_DESCRIPTION("xfstests generic/286 over a loopback NFS mount");
MODULE_LICENSE("GPL");
