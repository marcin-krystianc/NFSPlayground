// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/749 over a loopback NFS mount: the partial page after
 * the end of a file.
 *
 * mmap(2) maps whole pages, so a file whose length is not a multiple of
 * the page size leaves a tail inside the last mapped page. Upstream
 * checks three things about it: the tail reads as zeroes, writing into
 * the tail does not change the file's size (the data need never reach the
 * file, and what a later mapping sees is deliberately unspecified), and
 * going past that page is a SIGBUS.
 *
 * Over NFS the zero-fill is the client's: nfs_read_folio() has to clear
 * the part of the last folio beyond EOF rather than leave whatever the
 * page held, and the same range must not be written back as file data.
 *
 * Deviations: the SIGBUS case is expressed as the kernel sees it. A
 * kernel-mode access to an address the fault handler refuses does not
 * raise a signal; copy_from_user() takes the exception and reports the
 * bytes it could not copy, so the port requires the access to fail rather
 * than requiring a signal. Upstream's mount cycle before re-reading is
 * the fixture's page-cache invalidation, and the file is checked on the
 * server afterwards as well -- which is the strongest form of "the data
 * never reached the file".
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G749_ROOT	XFS_MNT "/g749"
#define G749_FILE	G749_ROOT "/file"
#define G749_SERVER	XFS_EXPORT "/g749/file"

#define G749_LEN	4000		/* not a page multiple */
#define G749_TAIL	(PAGE_SIZE - G749_LEN)

static void g749_remove_tree(void *unused)
{
	xfs_unlink(G749_FILE);
	xfs_rmdir_settled(G749_ROOT);
}

static void g749_one(struct kunit *test, bool sparse)
{
	const char *what = sparse ? "a sparse file" : "an allocated file";
	unsigned long addr, left;
	struct kstat before, after;
	struct file *f;
	u8 *buf;
	int i;

	xfs_unlink(G749_FILE);
	buf = kunit_kmalloc(test, PAGE_SIZE * 2, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G749_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
			       PTR_ERR(f));
	if (sparse) {
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G749_LEN), 0);
	} else {
		KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, G749_LEN), 0,
				    "%s: falloc failed", what);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_ASSERT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G749_FILE, &before), 0);
	KUNIT_ASSERT_EQ_MSG(test, before.size, (loff_t)G749_LEN,
			    "%s: the file is %lld bytes", what, before.size);

	addr = kunit_vm_mmap(test, f, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "%s: mmap failed", what);

	/* (a) the tail after EOF reads as zeroes */
	left = copy_from_user(buf, (void __user *)(addr + G749_LEN),
			      G749_TAIL);
	KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
			    "%s: reading the tail left %lu bytes", what, left);
	for (i = 0; i < G749_TAIL; i++)
		if (buf[i]) {
			KUNIT_FAIL(test,
				   "%s: byte %d of the tail is %02x, expected 00",
				   what, i, buf[i]);
			break;
		}

	/* (b) writing into the tail does not change the file */
	memset(buf, 0x5a, G749_TAIL);
	left = copy_to_user((void __user *)(addr + G749_LEN), buf, G749_TAIL);
	KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
			    "%s: writing the tail left %lu bytes", what, left);

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G749_FILE, &after), 0);
	KUNIT_EXPECT_EQ_MSG(test, after.size, before.size,
			    "%s: the file grew to %lld bytes after a write past EOF",
			    what, after.size);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G749_SERVER, &after), 0);
	KUNIT_EXPECT_EQ_MSG(test, after.size, before.size,
			    "%s: the server's copy grew to %lld bytes", what,
			    after.size);
}

static void the_tail_of_the_last_page_is_zero_and_stays_off_the_file(
		struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G749_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g749_remove_tree, NULL),
			0);

	g749_one(test, true);
	g749_one(test, false);
}

static void past_the_last_page_the_access_fails(struct kunit *test)
{
	unsigned long addr, left;
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G749_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g749_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G749_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G749_LEN), 0);

	/* map two pages of a one-page file: the second has nothing behind it */
	addr = kunit_vm_mmap(test, f, 0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mmap failed");

	left = copy_from_user(buf, (void __user *)(addr + PAGE_SIZE),
			      PAGE_SIZE);
	KUNIT_EXPECT_EQ_MSG(test, left, (unsigned long)PAGE_SIZE,
			    "reading past the last page of the file copied %lu bytes; it should have faulted",
			    PAGE_SIZE - left);

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, 2 * PAGE_SIZE), 0);
	filp_close(f, NULL);
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
	KUNIT_CASE(the_tail_of_the_last_page_is_zero_and_stays_off_the_file),
	KUNIT_CASE(past_the_last_page_the_access_fails),
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
