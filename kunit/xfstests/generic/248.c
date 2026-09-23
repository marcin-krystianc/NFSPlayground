// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/248 over a loopback NFS mount: pwrite whose source is a
 * mapping of the file being written.
 *
 * src/pwrite_mmap_blocked writes "01234" to a new file, maps those five
 * bytes MAP_SHARED, and then pwrites one byte from mapped_mem+2 to offset
 * 3 -- source and destination are the same page of the same file. It was
 * written for a hang: the write path holds the target page locked while
 * copying from a source that faults on the very same page.
 *
 * Over NFS the equivalent question is about nfs_write_begin()/
 * nfs_write_end() around a fault that has to be serviced by
 * nfs_vm_page_mkwrite() on the page the write already holds. The port
 * does the same one-byte write out of the mapping and then checks the
 * result upstream only checks by not hanging: byte 3 must have become
 * byte 2's value ('2'), everything else must be unchanged, and the
 * server must hold the same five bytes.
 *
 * The write goes through xfs_user_write(), which is pwrite(2)'s iterator
 * with a user source address; kernel_write() cannot express that.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>

#include "xfstests_nfs_fixture.h"

#define G248_ROOT	XFS_MNT "/g248"
#define G248_FILE	G248_ROOT "/test_file"
#define G248_SERVER	XFS_EXPORT "/g248/test_file"

#define G248_SIZE	5
#define G248_FROM	2
#define G248_TO		3

static void g248_remove_tree(void *unused)
{
	xfs_unlink(G248_FILE);
	xfs_rmdir_settled(G248_ROOT);
}

static void pwrite_from_a_mapping_of_the_same_page(struct kunit *test)
{
	static const char init[G248_SIZE] = { '0', '1', '2', '3', '4' };
	char want[G248_SIZE];
	unsigned long addr;
	struct file *f;
	loff_t pos = 0;
	char got[G248_SIZE + 1];

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G248_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g248_remove_tree, NULL),
			0);

	f = filp_open(G248_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, init, G248_SIZE, &pos),
			(ssize_t)G248_SIZE);

	addr = kunit_vm_mmap(test, f, 0, G248_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	pos = G248_TO;
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_user_write(f, (void __user *)(addr + G248_FROM),
					   1, &pos),
			    1L,
			    "pwrite 1 byte from %d to %d failed", G248_FROM,
			    G248_TO);

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G248_SIZE), 0);
	filp_close(f, NULL);

	memcpy(want, init, G248_SIZE);
	want[G248_TO] = init[G248_FROM];

	KUNIT_ASSERT_EQ(test, xfs_read_range(G248_FILE, got, G248_SIZE, 0),
			(ssize_t)G248_SIZE);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, G248_SIZE), 0,
			    "client holds %.5s, expected %.5s", got, want);

	KUNIT_ASSERT_EQ(test, xfs_read_range(G248_SERVER, got, G248_SIZE, 0),
			(ssize_t)G248_SIZE);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, G248_SIZE), 0,
			    "server holds %.5s, expected %.5s", got, want);
}

static int g248_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g248_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g248_cases[] = {
	KUNIT_CASE(pwrite_from_a_mapping_of_the_same_page),
	{}
};

static struct kunit_suite g248_suite = {
	.name		= "xfstests/generic/248",
	.suite_init	= g248_suite_init,
	.suite_exit	= g248_suite_exit,
	.test_cases	= g248_cases,
};

kunit_test_suites(&g248_suite);

MODULE_DESCRIPTION("xfstests generic/248 over a loopback NFS mount");
MODULE_LICENSE("GPL");
