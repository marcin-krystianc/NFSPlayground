// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/647 over a loopback NFS mount: page faults on a
 * mapping of the file being read or written.
 *
 * src/mmap-rw-fault sets up, five times over, a two-page file whose first
 * page is a hole and whose second page holds a known byte, maps both
 * pages MAP_PRIVATE, and then does one I/O whose user buffer is inside
 * that mapping:
 *
 *	pread  from page 1 into page 0 of the mapping, buffered
 *	pread  from page 1 into page 0 of the mapping, O_DIRECT
 *	pwrite from page 1 of the mapping to offset 0, buffered
 *	pwrite from page 1 of the mapping to offset 0, O_DIRECT
 *	pread  from the hole at offset 0 into page 0, O_DIRECT
 *
 * Each one makes the kernel fault a page of the same file while it is in
 * the middle of an I/O on that file. The direct cases are the sharp ones:
 * the write path invalidates the page cache for the range before issuing
 * the I/O, so a filesystem that faults the source in and then retries
 * with faults disabled can livelock.
 *
 * Over NFS the fault is serviced by nfs_readpage()/nfs_vm_page_mkwrite()
 * against the same inode the I/O holds, and the direct path is
 * nfs_direct_read()/nfs_direct_write() extracting pages from a mapping of
 * that same file. generic/729 adds the sixth case (a direct write of a
 * mapped page onto itself).
 *
 * Deviations: the buffer is a user address inside a kunit_vm_mmap()
 * mapping and the I/O goes through xfs_user_rw(), which is pread/pwrite's
 * iterator with the direct flag chosen per case. The comparison buffer is
 * an ordinary kernel one.
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

#define G647_ROOT	XFS_MNT "/g647"
#define G647_FILE	G647_ROOT "/mmap-rw-fault.tmp"

static void g647_remove_tree(void *unused)
{
	xfs_unlink(G647_FILE);
	xfs_rmdir_settled(G647_ROOT);
}

/*
 * upstream's init(): a file whose first page is a hole and whose second
 * page is filled with c, reopened and mapped MAP_PRIVATE over both pages.
 */
static struct file *g647_init(struct kunit *test, u8 c, int flags,
			      unsigned long *addr, u8 *page)
{
	struct file *f;
	loff_t pos = PAGE_SIZE;

	xfs_unlink(G647_FILE);
	memset(page, c, PAGE_SIZE);

	f = filp_open(G647_FILE, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT,
		      0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_direct_write(f, page, PAGE_SIZE, &pos),
			(ssize_t)PAGE_SIZE);
	filp_close(f, NULL);

	f = filp_open(G647_FILE, flags, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "reopen: %ld", PTR_ERR(f));

	*addr = kunit_vm_mmap(test, f, 0, 2 * PAGE_SIZE,
			      PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, *addr, 0UL, "kunit_vm_mmap failed");
	return f;
}

static void g647_done(struct kunit *test, struct file *f, unsigned long addr)
{
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, 2 * PAGE_SIZE), 0);
	filp_close(f, NULL);
}

/* does the first mapped page hold exactly want[]? */
static void g647_check(struct kunit *test, unsigned long addr, const u8 *want,
		       u8 *scratch, const char *what)
{
	unsigned long left = copy_from_user(scratch, (void __user *)addr,
					    PAGE_SIZE);

	KUNIT_ASSERT_EQ_MSG(test, left, 0UL, "%s: reading the mapping back left %lu bytes",
			    what, left);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(scratch, want, PAGE_SIZE), 0,
			    "%s: the mapped page holds the wrong bytes", what);
}

static void faults_on_the_files_own_mapping_are_handled(struct kunit *test)
{
	unsigned long addr;
	struct file *f;
	u8 *page, *scratch;
	loff_t pos;
	ssize_t n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G647_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g647_remove_tree, NULL),
			0);

	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	scratch = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, scratch);

	/* 1. buffered read into page 0 of the mapping */
	f = g647_init(test, 'a', O_RDWR, &addr, page);
	pos = PAGE_SIZE;
	n = xfs_user_rw(f, (void __user *)addr, PAGE_SIZE, &pos, false, false);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "buffered pread into the mapping returned %zd", n);
	g647_check(test, addr, page, scratch, "buffered pread");
	g647_done(test, f, addr);

	/* 2. the same read with O_DIRECT */
	f = g647_init(test, 'b', O_RDWR | O_DIRECT, &addr, page);
	pos = PAGE_SIZE;
	n = xfs_user_rw(f, (void __user *)addr, PAGE_SIZE, &pos, false, true);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "direct pread into the mapping returned %zd", n);
	g647_check(test, addr, page, scratch, "direct pread");
	g647_done(test, f, addr);

	/* 3. buffered write from page 1 of the mapping to offset 0 */
	f = g647_init(test, 'c', O_RDWR, &addr, page);
	pos = 0;
	n = xfs_user_rw(f, (void __user *)(addr + PAGE_SIZE), PAGE_SIZE, &pos,
			true, false);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "buffered pwrite from the mapping returned %zd", n);
	g647_check(test, addr, page, scratch, "buffered pwrite");
	g647_done(test, f, addr);

	/* 4. the same write with O_DIRECT */
	f = g647_init(test, 'd', O_RDWR | O_DIRECT, &addr, page);
	pos = 0;
	n = xfs_user_rw(f, (void __user *)(addr + PAGE_SIZE), PAGE_SIZE, &pos,
			true, true);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "direct pwrite from the mapping returned %zd", n);
	g647_check(test, addr, page, scratch, "direct pwrite");
	g647_done(test, f, addr);

	/* 5. a direct read from the hole at offset 0 */
	f = g647_init(test, 'e', O_RDWR | O_DIRECT, &addr, page);
	pos = 0;
	n = xfs_user_rw(f, (void __user *)addr, PAGE_SIZE, &pos, false, true);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
			    "direct pread from the hole returned %zd", n);
	memset(page, 0, PAGE_SIZE);
	g647_check(test, addr, page, scratch, "direct pread from a hole");
	g647_done(test, f, addr);
}

static int g647_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g647_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g647_cases[] = {
	KUNIT_CASE(faults_on_the_files_own_mapping_are_handled),
	{}
};

static struct kunit_suite g647_suite = {
	.name		= "xfstests/generic/647",
	.suite_init	= g647_suite_init,
	.suite_exit	= g647_suite_exit,
	.test_cases	= g647_cases,
};

kunit_test_suites(&g647_suite);

MODULE_DESCRIPTION("xfstests generic/647 over a loopback NFS mount");
MODULE_LICENSE("GPL");
