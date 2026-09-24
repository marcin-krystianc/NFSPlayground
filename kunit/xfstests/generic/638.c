// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/638 over a loopback NFS mount: pwritev whose source is
 * a mapping of the pages it is writing to.
 *
 * src/t_mmap_writev_overlap maps two pages of a file, pwrites 64 bytes of
 * 0xAA at the start of each page, and then pwritevs those two mapped
 * ranges back into the file at offset 2 * pagesize - 64 -- so the last
 * 64 bytes of the write land in the second mapped page, which is also one
 * of the sources. It is the regression test for fuse commit 4f06dd92b5d0
 * ("fuse: fix write deadlock"): the write path holds the destination page
 * while faulting the source, and they are the same page.
 *
 * Over NFS the equivalent is nfs_write_begin()/nfs_write_end() around a
 * copy whose source fault has to be serviced by nfs_vm_page_mkwrite() on
 * a page the write already owns. generic/248 does the one-byte version of
 * this within a single page; this one spans two pages and extends the
 * file, so the second half of the write is into a page that does not
 * exist yet.
 *
 * The bytes are checked afterwards, as upstream does, on the client and
 * on the server's own copy.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uio.h>

#include "xfstests_nfs_fixture.h"

#define G638_ROOT	XFS_MNT "/g638"
#define G638_FILE	G638_ROOT "/mmap-writev-overlap"
#define G638_SERVER	XFS_EXPORT "/g638/mmap-writev-overlap"

#define G638_COUNT	2		/* upstream's -c 2 */
#define G638_LEN	64		/* upstream's -l 64 */

static void g638_remove_tree(void *unused)
{
	xfs_unlink(G638_FILE);
	xfs_rmdir_settled(G638_ROOT);
}

static void g638_check(struct kunit *test, const char *path, loff_t off,
		       const char *which)
{
	u8 buf[G638_LEN];
	ssize_t n;
	int i;

	n = xfs_read_range(path, buf, G638_LEN, off);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G638_LEN,
			    "%s: read at %lld returned %zd", which, off, n);
	for (i = 0; i < G638_LEN; i++)
		if (buf[i] != 0xaa) {
			KUNIT_FAIL(test, "%s: byte %lld is %02x, expected aa",
				   which, off + i, buf[i]);
			return;
		}
}

static void writev_from_the_pages_it_writes_to(struct kunit *test)
{
	const loff_t mapsz = (loff_t)G638_COUNT * PAGE_SIZE;
	const loff_t target = mapsz - G638_LEN;
	struct iovec iov[G638_COUNT];
	unsigned long addr;
	struct file *f;
	loff_t pos;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G638_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g638_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G638_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xaa, G638_LEN);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G638_FILE, "", 0), 0);
	f = filp_open(G638_FILE, O_RDWR, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	addr = kunit_vm_mmap(test, f, 0, mapsz, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	/* 64 bytes of 0xAA at the start of each mapped page */
	for (i = 0; i < G638_COUNT; i++) {
		pos = (loff_t)i * PAGE_SIZE;
		KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, buf, G638_LEN, &pos),
				    (ssize_t)G638_LEN,
				    "seeding page %d failed", i);
	}

	/* and write both of them back, overlapping the second page */
	for (i = 0; i < G638_COUNT; i++) {
		iov[i].iov_base = (void __user *)(addr + (unsigned long)i *
						  PAGE_SIZE);
		iov[i].iov_len = G638_LEN;
	}
	pos = target;
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_user_writev(f, iov, G638_COUNT, &pos),
			    (ssize_t)(G638_COUNT * G638_LEN),
			    "the overlapping pwritev did not complete");

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, mapsz), 0);
	filp_close(f, NULL);

	for (i = 0; i < G638_COUNT; i++) {
		loff_t off = target + (loff_t)i * G638_LEN;

		g638_check(test, G638_FILE, off, "client");
		g638_check(test, G638_SERVER, off, "server");
	}
}

static int g638_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g638_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g638_cases[] = {
	KUNIT_CASE(writev_from_the_pages_it_writes_to),
	{}
};

static struct kunit_suite g638_suite = {
	.name		= "xfstests/generic/638",
	.suite_init	= g638_suite_init,
	.suite_exit	= g638_suite_exit,
	.test_cases	= g638_cases,
};

kunit_test_suites(&g638_suite);

MODULE_DESCRIPTION("xfstests generic/638 over a loopback NFS mount");
MODULE_LICENSE("GPL");
