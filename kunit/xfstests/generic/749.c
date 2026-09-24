// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/749 over a loopback NFS mount: the partial page after
 * the end of a file.
 *
 * mmap(2) maps whole pages, so a file whose length is not a multiple of
 * the page size leaves a tail inside the last mapped page. Upstream's
 * do_mmap_tests() creates a file (truncate, or falloc when not sparse),
 * pwrites 0xaa over part of it, and then checks:
 *
 *	a) the tail reads as zeroes through a fresh mapping;
 *	b) writing into the tail changes neither the file's contents nor
 *	   its size;
 *	c) reading the whole last page through a mapping is not a SIGBUS;
 *	d) reading, and writing, through a mapping that extends 10 bytes
 *	   past that page is a SIGBUS, and leaves the size alone.
 *
 * It runs six parameter sets at the filesystem's block size; each is a
 * test case here, with upstream's file lengths, offsets and lengths.
 *
 * Over NFS the zero-fill is the client's: nfs_read_folio() has to clear
 * the part of the last folio beyond EOF rather than leave whatever the
 * page held, and the same range must not be written back as file data.
 *
 * Deviations: a kernel-mode access to an address the fault handler
 * refuses does not raise a signal; copy_{from,to}_user() takes the
 * exception and reports the bytes it could not copy, so SIGBUS is "the
 * access failed" and no SIGBUS is "the access copied everything".
 * Upstream's mount cycles are an fsync and invalidate_inode_pages2() of
 * the file's page cache; its md5sum is a byte comparison against a copy
 * read before; the tail write uses 0x5a. Three upstream steps have no
 * kernel-side effect and are not ported: "mwrite $map_len 0 $map_len"
 * passes the length where the file belongs and always succeeds, and the
 * two xfs_io mread/mwrite calls past a map_len mapping are refused by
 * xfs_io's own range check before any access.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G749_ROOT	XFS_MNT "/g749"
#define G749_FILE	G749_ROOT "/file"
#define G749_MAX	(128 * 1024)	/* past every map_len + 10 below */

static void g749_remove_tree(void *unused)
{
	xfs_unlink(G749_FILE);
	xfs_rmdir_settled(G749_ROOT);
}

/* _scratch_cycle_mount, as far as this file's page cache is concerned */
static void g749_cycle(struct kunit *test)
{
	struct file *f = filp_open(G749_FILE, O_RDWR, 0);

	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_ASSERT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);
	filp_close(f, NULL);
}

static loff_t g749_size(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G749_FILE, &st), 0);
	return st.size;
}

/* map [0, map_len) of the file, shared, read-write or read-only */
static unsigned long g749_map(struct kunit *test, size_t map_len, bool w,
			      struct file **fp)
{
	struct file *f = filp_open(G749_FILE, w ? O_RDWR : O_RDONLY, 0);
	unsigned long addr;

	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	addr = kunit_vm_mmap(test, f, 0, map_len,
			     PROT_READ | (w ? PROT_WRITE : 0), MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mmap of %zu bytes failed",
			    map_len);
	*fp = f;
	return addr;
}

static void g749_unmap(struct kunit *test, unsigned long addr, size_t len,
		       struct file *f)
{
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, len), 0);
	filp_close(f, NULL);
}

/* _mread / mwrite of [0, len) through a map_len mapping: bytes not copied */
static unsigned long g749_maccess(struct kunit *test, size_t map_len,
				  size_t len, bool write, u8 *buf)
{
	struct file *f;
	unsigned long addr = g749_map(test, map_len, write, &f);
	unsigned long left;

	if (write) {
		memset(buf, 0x5a, len);
		left = copy_to_user((void __user *)addr, buf, len);
	} else {
		left = copy_from_user(buf, (void __user *)addr, len);
	}
	g749_unmap(test, addr, map_len, f);
	return left;
}

static void g749_read_all(struct kunit *test, u8 *buf, loff_t len)
{
	KUNIT_ASSERT_EQ(test, xfs_read_range(G749_FILE, buf, len, 0),
			(ssize_t)len);
}

static void do_mmap_tests(struct kunit *test, loff_t file_len, loff_t offset,
			  size_t len, bool sparse)
{
	loff_t new_filelen, map_len;
	unsigned long addr, left;
	struct file *f;
	u8 *buf, *orig, *post;
	loff_t pos = offset;
	size_t i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G749_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g749_remove_tree, NULL),
			0);
	buf = kunit_kzalloc(test, G749_MAX, GFP_KERNEL);
	orig = kunit_kzalloc(test, G749_MAX, GFP_KERNEL);
	post = kunit_kzalloc(test, G749_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, orig);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, post);

	/* setup_zeroed_file */
	f = filp_open(G749_FILE, O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	if (sparse)
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, file_len), 0);
	else
		KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, file_len), 0);

	/* pwrite -S 0xaa -b 512 $offset $len */
	memset(buf, 0xaa, 512);
	while (pos < offset + len) {
		size_t n = min_t(loff_t, offset + len - pos, 512);

		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, n, &pos),
				(ssize_t)n);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	new_filelen = g749_size(test);
	map_len = round_up(new_filelen, PAGE_SIZE);
	KUNIT_ASSERT_LE(test, map_len + 10, (loff_t)G749_MAX);
	g749_read_all(test, orig, new_filelen);

	if (map_len > new_filelen) {
		size_t tail = map_len - new_filelen;

		/* a) the tail after EOF reads as zeroes */
		g749_cycle(test);
		addr = g749_map(test, map_len, false, &f);
		left = copy_from_user(buf, (void __user *)(addr + new_filelen),
				      tail);
		g749_unmap(test, addr, map_len, f);
		KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
				    "reading the tail left %lu bytes", left);
		for (i = 0; i < tail; i++)
			if (buf[i]) {
				KUNIT_FAIL(test,
					   "Zero-fill expectations with mmap() not respected: byte %lld is %02x",
					   new_filelen + (loff_t)i, buf[i]);
				break;
			}

		/* b) writing into the tail changes neither contents nor size */
		g749_cycle(test);
		addr = g749_map(test, map_len, true, &f);
		memset(buf, 0x5a, tail);
		left = copy_to_user((void __user *)(addr + new_filelen), buf,
				    tail);
		g749_unmap(test, addr, map_len, f);
		KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
				    "writing the tail left %lu bytes", left);
		g749_cycle(test);
		KUNIT_EXPECT_EQ_MSG(test, g749_size(test), new_filelen,
				    "mmap() write up to page boundary should not change actual file size");
		g749_read_all(test, post, new_filelen);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(orig, post, new_filelen), 0,
				    "mmap() write up to page boundary should not change actual file contents");
	}

	g749_cycle(test);
	new_filelen = g749_size(test);
	map_len = round_up(new_filelen, PAGE_SIZE);

	/* c) up to the page boundary: no SIGBUS */
	left = g749_maccess(test, map_len, map_len, false, buf);
	KUNIT_EXPECT_EQ_MSG(test, left, 0UL,
			    "Not expecting SIGBUS when reading up to page boundary (%lu bytes not read)",
			    left);

	/* d) 10 bytes past it: SIGBUS, reading and writing */
	left = g749_maccess(test, map_len + 10, map_len + 10, false, buf);
	KUNIT_EXPECT_NE_MSG(test, left, 0UL,
			    "Expected SIGBUS when mmap() reading beyond page boundary");
	left = g749_maccess(test, map_len + 10, map_len + 10, true, buf);
	KUNIT_EXPECT_NE_MSG(test, left, 0UL,
			    "Expected SIGBUS when mmap() writing beyond page boundary");

	KUNIT_EXPECT_EQ_MSG(test, g749_size(test), new_filelen,
			    "reading or writing beyond file size up to mmap() page boundary should not change file size");
}

static void len_512_off_3_len_5(struct kunit *test)
{
	do_mmap_tests(test, 512, 3, 5, false);
}

static void len_11k_off_0_len_12291(struct kunit *test)
{
	do_mmap_tests(test, 11 * 1024, 0, 4096 * 3 + 3, false);
}

static void len_16k_off_0_len_16387(struct kunit *test)
{
	do_mmap_tests(test, 16 * 1024, 0, 16384 + 3, false);
}

static void len_16k_off_16374_len_16404(struct kunit *test)
{
	do_mmap_tests(test, 16 * 1024, 16384 - 10, 16384 + 20, false);
}

static void len_64k_off_0_len_65539(struct kunit *test)
{
	do_mmap_tests(test, 64 * 1024, 0, 65536 + 3, false);
}

static void sparse_len_4k_off_4090_len_30(struct kunit *test)
{
	do_mmap_tests(test, 4 * 1024, 4090, 30, true);
}

static int g749_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g749_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g749_cases[] = {
	KUNIT_CASE(len_512_off_3_len_5),
	KUNIT_CASE(len_11k_off_0_len_12291),
	KUNIT_CASE(len_16k_off_0_len_16387),
	KUNIT_CASE(len_16k_off_16374_len_16404),
	KUNIT_CASE(len_64k_off_0_len_65539),
	KUNIT_CASE(sparse_len_4k_off_4090_len_30),
	{}
};

static struct kunit_suite g749_suite = {
	.name		= "xfstests/generic/749",
	.suite_init	= g749_suite_init,
	.suite_exit	= g749_suite_exit,
	.test_cases	= g749_cases,
};

kunit_test_suites(&g749_suite);

MODULE_DESCRIPTION("xfstests generic/749 over a loopback NFS mount");
MODULE_LICENSE("GPL");
