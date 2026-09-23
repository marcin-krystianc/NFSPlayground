// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/729 over a loopback NFS mount: a direct write of a
 * mapped page onto itself.
 *
 * generic/729 runs src/mmap-rw-fault with -2, which is generic/647's
 * five cases plus a sixth: map two pages of a file MAP_PRIVATE, then
 * pwrite page 1 of the mapping back to the same file offset it is mapped
 * from, with O_DIRECT. Upstream's own comment says why that is the
 * dangerous one -- "the kernel will invalidate the page cache before
 * carrying out the write, so filesystems that fault in the page and then
 * carry out the direct I/O write with page faults disabled will never
 * make any progress".
 *
 * Over NFS the invalidation is nfs_file_direct_write()'s
 * invalidate_inode_pages2_range() over exactly the range whose source
 * page the write is about to fault. If that combination livelocked, this
 * case would hang rather than fail, and KUnit's timeout is what would
 * report it.
 *
 * The other five cases are generic/647's suite; this port is the case
 * that -2 adds, rather than a second copy of all six.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G729_ROOT	XFS_MNT "/g729"
#define G729_FILE	G729_ROOT "/mmap-rw-fault.tmp"
#define G729_SERVER	XFS_EXPORT "/g729/mmap-rw-fault.tmp"

static void g729_remove_tree(void *unused)
{
	xfs_unlink(G729_FILE);
	xfs_rmdir_settled(G729_ROOT);
}

static void a_direct_write_of_a_mapped_page_onto_itself(struct kunit *test)
{
	unsigned long addr;
	struct file *f;
	u8 *page, *got;
	loff_t pos;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G729_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g729_remove_tree, NULL),
			0);

	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	memset(page, 'f', PAGE_SIZE);

	/* upstream's init('f', O_RDWR | O_DIRECT) */
	f = filp_open(G729_FILE, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT,
		      0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create: %ld", PTR_ERR(f));
	pos = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, xfs_direct_write(f, page, PAGE_SIZE, &pos),
			(ssize_t)PAGE_SIZE);
	filp_close(f, NULL);

	f = filp_open(G729_FILE, O_RDWR | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "reopen: %ld", PTR_ERR(f));
	addr = kunit_vm_mmap(test, f, 0, 2 * PAGE_SIZE,
			     PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	/* write the second mapped page back over the offset it maps */
	pos = PAGE_SIZE;
	n = xfs_user_rw(f, (void __user *)(addr + PAGE_SIZE), PAGE_SIZE, &pos,
			true, true);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "the direct write returned %zd", n);

	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, 2 * PAGE_SIZE), 0);
	filp_close(f, NULL);

	/* and the page still holds what it held */
	KUNIT_ASSERT_EQ(test,
			xfs_read_range(G729_SERVER, got, PAGE_SIZE, PAGE_SIZE),
			(ssize_t)PAGE_SIZE);
	for (i = 0; i < PAGE_SIZE; i++)
		if (got[i] != 'f') {
			KUNIT_FAIL(test, "server byte %d is %02x, expected 'f'",
				   i, got[i]);
			break;
		}
}

static int g729_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g729_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g729_cases[] = {
	KUNIT_CASE(a_direct_write_of_a_mapped_page_onto_itself),
	{}
};

static struct kunit_suite g729_suite = {
	.name		= "xfstests/generic/729",
	.suite_init	= g729_suite_init,
	.suite_exit	= g729_suite_exit,
	.test_cases	= g729_cases,
};

kunit_test_suites(&g729_suite);

MODULE_DESCRIPTION("xfstests generic/729 over a loopback NFS mount");
MODULE_LICENSE("GPL");
