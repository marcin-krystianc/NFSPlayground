// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/080 over a loopback NFS mount: mtime and ctime after a
 * mapped write.
 *
 * Upstream writes 4k, fsyncs, samples mtime/ctime, sleeps a second, then
 * mmaps the file and does "mread 0 4k" followed by "mwrite 0 4k". When
 * xfs_io exits the mapping is torn down and the file closed; both
 * timestamps must have moved. A read through the mapping must not be what
 * moves them -- the read comes first precisely so that a filesystem which
 * dirties on fault-for-read is not credited with the update.
 *
 * Over NFS the timestamps live on the server, so the check is only
 * meaningful once the mapped write has reached it. That is what the
 * munmap/close pair does here, exactly as xfs_io's exit does upstream:
 * nfs_file_flush() -> nfs_wb_all() on close pushes the page
 * nfs_vm_page_mkwrite() dirtied, and the WRITE is what makes the server
 * move mtime and ctime. The samples themselves are taken with
 * AT_STATX_FORCE_SYNC so they are the server's values and not the
 * client's cached ones.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G080_ROOT	XFS_MNT "/g080"
#define G080_FILE	G080_ROOT "/mmap_mtime_testfile"
#define G080_LEN	4096

static void g080_remove_tree(void *unused)
{
	xfs_unlink(G080_FILE);
	xfs_rmdir_settled(G080_ROOT);
}

static bool g080_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void mapped_write_updates_mtime_and_ctime(struct kunit *test)
{
	struct kstat before, after;
	unsigned long addr, left;
	struct file *f;
	u8 *scratch;
	loff_t pos = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G080_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g080_remove_tree, NULL),
			0);

	scratch = kunit_kmalloc(test, G080_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, scratch);
	memset(scratch, 0x63, G080_LEN);

	/* pattern the file and get it onto the server */
	f = filp_open(G080_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, scratch, G080_LEN, &pos),
			(ssize_t)G080_LEN);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G080_FILE, &before), 0);

	/* upstream's sleep 1, scaled: enough to separate two timestamps */
	msleep(20);

	addr = kunit_vm_mmap(test, f, 0, G080_LEN, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	/* mread first: a read through the mapping must not be the thing
	 * that moves the timestamps, which is why upstream orders it this
	 * way. Its result is checked so the fault actually happens.
	 */
	left = copy_from_user(scratch, (void __user *)addr, G080_LEN);
	KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
			    "mapped read left %lu bytes unread", left);
	KUNIT_EXPECT_EQ(test, scratch[0], 0x63);

	memset(scratch, 0x64, G080_LEN);
	left = copy_to_user((void __user *)addr, scratch, G080_LEN);
	KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
			    "mapped write left %lu bytes unwritten", left);

	/* xfs_io's exit: drop the mapping, then close, which flushes */
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G080_LEN), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G080_FILE, &after), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g080_after(&after.mtime, &before.mtime),
			      "mtime not updated: %lld.%09ld -> %lld.%09ld",
			      before.mtime.tv_sec, before.mtime.tv_nsec,
			      after.mtime.tv_sec, after.mtime.tv_nsec);
	KUNIT_EXPECT_TRUE_MSG(test, g080_after(&after.ctime, &before.ctime),
			      "ctime not updated: %lld.%09ld -> %lld.%09ld",
			      before.ctime.tv_sec, before.ctime.tv_nsec,
			      after.ctime.tv_sec, after.ctime.tv_nsec);

	/* and the mapped bytes are the ones the server now holds */
	KUNIT_ASSERT_EQ(test,
			xfs_read_range(XFS_EXPORT "/g080/mmap_mtime_testfile",
				       scratch, G080_LEN, 0),
			(ssize_t)G080_LEN);
	KUNIT_EXPECT_EQ_MSG(test, scratch[0], 0x64,
			    "server byte 0 is %02x after the mapped write",
			    scratch[0]);
	KUNIT_EXPECT_EQ_MSG(test, scratch[G080_LEN - 1], 0x64,
			    "server byte %d is %02x after the mapped write",
			    G080_LEN - 1, scratch[G080_LEN - 1]);
}

static int g080_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g080_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g080_cases[] = {
	KUNIT_CASE(mapped_write_updates_mtime_and_ctime),
	{}
};

static struct kunit_suite g080_suite = {
	.name		= "xfstests/generic/080",
	.suite_init	= g080_suite_init,
	.suite_exit	= g080_suite_exit,
	.test_cases	= g080_cases,
};

kunit_test_suites(&g080_suite);

MODULE_DESCRIPTION("xfstests generic/080 over a loopback NFS mount");
MODULE_LICENSE("GPL");
