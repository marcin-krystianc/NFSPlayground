// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/439 over a loopback NFS mount: writes that overlap a
 * punched range, and a punch on an empty file, then overlapping writes.
 *
 * Upstream's regression was btrfs losing data when a write followed a
 * punch hole that reached past EOF. It builds two files:
 *
 *   f:  100K of 0xaa, punch 60K..150K (past EOF, size stays 100K), then
 *       0xbb over 50K..150K and 0xcc over 100K..150K.
 *   f2: punch 695K..1515K on an empty file, then 0xaa over 1008K..1315K,
 *       0xbb over 1073K..1703K and 0xcc over 1068K..1527K.
 *
 * and compares an od of each after a mount cycle.
 *
 * Over NFS each punch is a DEALLOCATE RPC (nfs42_proc_deallocate()) and
 * each write goes through the page cache and is flushed on close.
 *
 * Deviations: the mount cycle is replaced by reading the file through the
 * tmpfs export (the server's own bytes), as in the generic/029 port, plus
 * a client read after invalidate_inode_pages2(). The od output is replaced
 * by a per-range model of the expected bytes.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "xfstests_nfs_fixture.h"

#define G439_ROOT	XFS_MNT "/g439"
#define K		1024

/* one byte value over [start, end) */
struct g439_range {
	loff_t	start, end;
	u8	val;
};

static void g439_remove_tree(void *unused)
{
	xfs_unlink(G439_ROOT "/f");
	xfs_unlink(G439_ROOT "/f2");
	xfs_rmdir_settled(G439_ROOT);
}

static void g439_pwrite(struct kunit *test, const char *path, u8 val,
			loff_t off, size_t len)
{
	struct file *f;
	ssize_t n;
	u8 *buf;

	/* one write of the whole range, as xfs_io's "-b len" does */
	buf = kvmalloc(len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, val, len);
	f = filp_open(path, O_RDWR, 0);
	if (IS_ERR(f)) {
		kvfree(buf);
		KUNIT_FAIL_AND_ABORT(test, "open %s: %ld", path, PTR_ERR(f));
	}
	n = kernel_write(f, buf, len, &off);
	filp_close(f, NULL);
	kvfree(buf);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len, "pwrite 0x%02x to %s",
			    val, path);
}

static void g439_fpunch(struct kunit *test, const char *path, loff_t off,
			loff_t len)
{
	struct file *f;
	int err;

	f = filp_open(path, O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open %s: %ld", path,
			       PTR_ERR(f));
	err = vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			    off, len);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "fpunch %lld %lld on %s", off, len,
			    path);
}

static void g439_verify(struct kunit *test, const char *path,
			const struct g439_range *r, int nr, const char *which)
{
	loff_t size = r[nr - 1].end;
	struct kstat st;
	u8 *buf;
	int i;

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, size, "%s %s: size", which, path);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < nr; i++) {
		loff_t pos;

		for (pos = r[i].start; pos < r[i].end; pos += PAGE_SIZE) {
			size_t n = min_t(loff_t, r[i].end - pos, PAGE_SIZE);
			size_t j;

			KUNIT_ASSERT_EQ(test, xfs_read_range(path, buf, n, pos),
					(ssize_t)n);
			for (j = 0; j < n; j++)
				if (buf[j] != r[i].val) {
					KUNIT_FAIL(test,
						   "%s %s: byte %lld is %02x, expected %02x",
						   which, path, pos + j, buf[j],
						   r[i].val);
					return;
				}
		}
	}
}

static void g439_invalidate(struct kunit *test, const char *path)
{
	struct file *f;

	f = filp_open(path, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);
	filp_close(f, NULL);
}

static void writes_over_punched_ranges_survive(struct kunit *test)
{
	/* upstream's od output, as ranges */
	static const struct g439_range f_want[] = {
		{ 0,        50 * K,  0xaa },
		{ 50 * K,   100 * K, 0xbb },
		{ 100 * K,  150 * K, 0xcc },
	};
	static const struct g439_range f2_want[] = {
		{ 0,         1008 * K, 0x00 },
		{ 1008 * K,  1068 * K, 0xaa },
		{ 1068 * K,  1527 * K, 0xcc },
		{ 1527 * K,  1703 * K, 0xbb },
	};

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G439_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g439_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G439_ROOT "/f", "", 0), 0);
	g439_pwrite(test, G439_ROOT "/f", 0xaa, 0, 100 * K);
	g439_fpunch(test, G439_ROOT "/f", 60 * K, 90 * K);
	g439_pwrite(test, G439_ROOT "/f", 0xbb, 50 * K, 100 * K);
	g439_pwrite(test, G439_ROOT "/f", 0xcc, 100 * K, 50 * K);

	g439_fpunch(test, G439_ROOT "/f2", 695 * K, 820 * K);
	g439_pwrite(test, G439_ROOT "/f2", 0xaa, 1008 * K, 307 * K);
	g439_pwrite(test, G439_ROOT "/f2", 0xbb, 1073 * K, 630 * K);
	g439_pwrite(test, G439_ROOT "/f2", 0xcc, 1068 * K, 459 * K);

	/* upstream's _scratch_cycle_mount, then od of each file */
	g439_verify(test, XFS_EXPORT "/g439/f", f_want, ARRAY_SIZE(f_want),
		    "SERVER");
	g439_verify(test, XFS_EXPORT "/g439/f2", f2_want,
		    ARRAY_SIZE(f2_want), "SERVER");
	g439_invalidate(test, G439_ROOT "/f");
	g439_invalidate(test, G439_ROOT "/f2");
	g439_verify(test, G439_ROOT "/f", f_want, ARRAY_SIZE(f_want),
		    "client");
	g439_verify(test, G439_ROOT "/f2", f2_want, ARRAY_SIZE(f2_want),
		    "client");
}

static int g439_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g439_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g439_cases[] = {
	KUNIT_CASE(writes_over_punched_ranges_survive),
	{}
};

static struct kunit_suite g439_suite = {
	.name		= "xfstests/generic/439",
	.suite_init	= g439_suite_init,
	.suite_exit	= g439_suite_exit,
	.test_cases	= g439_cases,
};

kunit_test_suites(&g439_suite);

MODULE_DESCRIPTION("xfstests generic/439 over a loopback NFS mount");
MODULE_LICENSE("GPL");
