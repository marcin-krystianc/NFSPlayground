// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/428 over a loopback NFS mount: a read-only shared
 * mapping sees data written with pwrite() after the mapping faulted.
 *
 * src/t_mmap_stale_pmd truncates a file to 4 MiB, maps the second 2 MiB
 * read-only and MAP_SHARED, reads the first byte of the mapping (a fault
 * on a hole), then pwrite()s "HELLO WORLD!" at 2 MiB and compares the
 * mapping with the buffer. The DAX bug it was written for left a huge
 * zero page mapped after the write, so the mapping read back zeroes.
 *
 * Over NFS the read fault is nfs_read_folio() on a range the server has
 * no data for, and the pwrite() is nfs_write_begin()/nfs_write_end() on
 * the same page-cache folio, so the mapping must show the new bytes
 * without any flush.
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

#define G428_ROOT	XFS_MNT "/g428"
#define G428_FILE	G428_ROOT "/testfile"

#define MIB		(1024 * 1024)

static void g428_remove_tree(void *unused)
{
	xfs_unlink(G428_FILE);
	xfs_rmdir_settled(G428_ROOT);
}

static void mapping_sees_pwrite_after_hole_fault(struct kunit *test)
{
	static const char hello[] = "HELLO WORLD!";
	char got[sizeof(hello) - 1];
	unsigned long addr;
	struct file *f;
	loff_t pos;
	u8 a;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G428_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g428_remove_tree, NULL),
			0);

	f = filp_open(G428_FILE, O_RDWR | O_CREAT, 0600);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 4 * MIB), 0);

	addr = kunit_vm_mmap(test, f, 0, 2 * MIB, PROT_READ, MAP_SHARED,
			     2 * MIB);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mmap failed");

	/* fault the hole in */
	KUNIT_ASSERT_EQ(test, copy_from_user(&a, (void __user *)addr, 1), 0UL);
	KUNIT_EXPECT_EQ(test, a, 0);

	pos = 2 * MIB;
	KUNIT_ASSERT_EQ(test, kernel_write(f, hello, sizeof(hello) - 1, &pos),
			(ssize_t)(sizeof(hello) - 1));

	KUNIT_ASSERT_EQ(test,
			copy_from_user(got, (void __user *)addr, sizeof(got)),
			0UL);
	KUNIT_EXPECT_MEMEQ_MSG(test, got, hello, sizeof(got),
			       "the mapping still shows the pre-write page");

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, 2 * MIB), 0);
	filp_close(f, NULL);
}

static int g428_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g428_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g428_cases[] = {
	KUNIT_CASE(mapping_sees_pwrite_after_hole_fault),
	{}
};

static struct kunit_suite g428_suite = {
	.name		= "xfstests/generic/428",
	.suite_init	= g428_suite_init,
	.suite_exit	= g428_suite_exit,
	.test_cases	= g428_cases,
};

kunit_test_suites(&g428_suite);

MODULE_DESCRIPTION("xfstests generic/428 over a loopback NFS mount");
MODULE_LICENSE("GPL");
