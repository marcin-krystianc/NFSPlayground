// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/086 over a loopback NFS mount: a write into a range that
 * was written, then allocated, then written again.
 *
 * Upstream's sequence on one file (tests/generic/086):
 *
 *	pwrite -S 0xaa 4096 2048
 *	falloc 0 131072
 *	pwrite -S 0xbb 65536 2048
 *	fsync
 *	echo 3 > /proc/sys/vm/drop_caches
 *	pwrite -S 0xdd 67584 2048
 *	hexdump
 *
 * It is a regression test for an ext4 extent-status-tree bug ("ext4: Fix
 * data corruption caused by unwritten and delayed extents") where the
 * second write into the block holding 0xbb came back as a fresh zeroed
 * buffer and overwrote it. The test runs on any filesystem.
 *
 * Over NFS the interesting part is the same shape with different
 * machinery: the ALLOCATE (falloc) extends the file past the dirty 0xaa
 * page, and after the cache drop the 0xdd write lands on a page the client
 * no longer holds, so nfs_write_begin() has to fetch the rest of that page
 * from the server rather than assume it is zero. Dropping the cache is
 * invalidate_inode_pages2() on the file's mapping, which is what
 * drop_caches reduces to for a single NFS inode.
 *
 * The golden hexdump is replaced by a model of the same bytes, checked
 * both through the client and against the server's own copy through the
 * tmpfs export.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G086_ROOT	XFS_MNT "/g086"
#define G086_FILE	G086_ROOT "/testfile"
#define G086_SERVER	XFS_EXPORT "/g086/testfile"

#define G086_SIZE	131072
#define G086_CHUNK	2048

static void g086_remove_tree(void *unused)
{
	xfs_unlink(G086_FILE);
	xfs_rmdir_settled(G086_ROOT);
}

/* the file upstream's hexdump describes, one byte at a time */
static u8 g086_expected(loff_t off)
{
	if (off >= 4096 && off < 4096 + G086_CHUNK)
		return 0xaa;
	if (off >= 65536 && off < 65536 + G086_CHUNK)
		return 0xbb;
	if (off >= 67584 && off < 67584 + G086_CHUNK)
		return 0xdd;
	return 0x00;
}

static void g086_pwrite(struct kunit *test, struct file *f, u8 val, loff_t off)
{
	u8 *buf;
	loff_t pos = off;

	buf = kunit_kmalloc(test, G086_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, val, G086_CHUNK);
	KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, buf, G086_CHUNK, &pos),
			    (ssize_t)G086_CHUNK,
			    "pwrite -S 0x%02x %lld %d failed", val, off,
			    G086_CHUNK);
}

static void g086_verify(struct kunit *test, const char *path, const char *which)
{
	loff_t off;
	u8 *buf;

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (off = 0; off < G086_SIZE; off += PAGE_SIZE) {
		ssize_t n = xfs_read_range(path, buf, PAGE_SIZE, off);
		int i;

		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
				    "%s: read at %lld returned %zd", which,
				    off, n);
		for (i = 0; i < PAGE_SIZE; i++)
			if (buf[i] != g086_expected(off + i)) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   which, off + i, buf[i],
					   g086_expected(off + i));
				return;
			}
	}
}

static void a_write_after_allocate_and_cache_drop_keeps_its_neighbour(
		struct kunit *test)
{
	struct kstat st;
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G086_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g086_remove_tree, NULL),
			0);

	f = filp_open(G086_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	g086_pwrite(test, f, 0xaa, 4096);

	KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, G086_SIZE), 0,
			    "falloc 0 %d failed", G086_SIZE);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G086_FILE, &st), 0);
	KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)G086_SIZE,
			    "ALLOCATE left the size at %lld", st.size);

	g086_pwrite(test, f, 0xbb, 65536);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	/* upstream's drop_caches, narrowed to this inode */
	KUNIT_ASSERT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);

	g086_pwrite(test, f, 0xdd, 67584);

	g086_verify(test, G086_FILE, "client");

	filp_close(f, NULL);
	g086_verify(test, G086_SERVER, "server");
}

static int g086_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g086_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g086_cases[] = {
	KUNIT_CASE(a_write_after_allocate_and_cache_drop_keeps_its_neighbour),
	{}
};

static struct kunit_suite g086_suite = {
	.name		= "xfstests/generic/086",
	.suite_init	= g086_suite_init,
	.suite_exit	= g086_suite_exit,
	.test_cases	= g086_cases,
};

kunit_test_suites(&g086_suite);

MODULE_DESCRIPTION("xfstests generic/086 over a loopback NFS mount");
MODULE_LICENSE("GPL");
