// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/215 over a loopback NFS mount: c/mtime after a mapped
 * write (kernel.org bug 2645).
 *
 * Upstream creates a 2 MiB file with dd, samples mtime and ctime, sleeps a
 * second, maps the first page read-write and writes through it, then
 * requires both timestamps to have moved by a non-zero number of seconds.
 *
 * generic/080 checks the same property; this one differs in what it does
 * first. There is no mapped read before the write, and the file is created
 * by a plain write of two megabytes rather than a single page, so the page
 * that gets dirtied through the mapping is one of many already written
 * back. Over NFS that matters: the mapped write has to produce its own
 * WRITE (via nfs_vm_page_mkwrite() recording the dirty range) rather than
 * riding on the earlier ones, or the server's timestamps never move.
 *
 * Deviations: upstream's sleep(1) is scaled down, and the timestamps are
 * compared as full timespec64s rather than whole seconds -- a stricter
 * check, since a client that updated them only in its own cache would
 * still fail the FORCE_SYNC stat used here.
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

#define G215_ROOT	XFS_MNT "/g215"
#define G215_FILE	G215_ROOT "/tst.mmap"

#define G215_SIZE	(2 * 1024 * 1024)	/* dd count=4096 at bs=512 */
#define G215_CHUNK	65536
#define G215_MAPLEN	4096

static void g215_remove_tree(void *unused)
{
	xfs_unlink(G215_FILE);
	xfs_rmdir_settled(G215_ROOT);
}

static bool g215_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void a_mapped_write_moves_both_times(struct kunit *test)
{
	struct kstat before, after;
	unsigned long addr, left;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G215_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g215_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G215_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0, G215_CHUNK);

	/* dd if=/dev/zero of=$testfile count=4096 */
	f = filp_open(G215_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	while (pos < G215_SIZE)
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G215_CHUNK, &pos),
				(ssize_t)G215_CHUNK);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G215_FILE, &before), 0);
	msleep(20);	/* upstream's sleep 2, scaled */

	addr = kunit_vm_mmap(test, f, 0, G215_MAPLEN, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	memset(buf, 0x2a, G215_MAPLEN);
	left = copy_to_user((void __user *)addr, buf, G215_MAPLEN);
	KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
			    "mapped write left %lu bytes unwritten", left);

	/* teardown is what flushes the mapped write to the server */
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G215_MAPLEN), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G215_FILE, &after), 0);
	KUNIT_EXPECT_TRUE_MSG(test, g215_after(&after.mtime, &before.mtime),
			      "mtime not updated after the mapped write: %lld.%09ld -> %lld.%09ld",
			      before.mtime.tv_sec, before.mtime.tv_nsec,
			      after.mtime.tv_sec, after.mtime.tv_nsec);
	KUNIT_EXPECT_TRUE_MSG(test, g215_after(&after.ctime, &before.ctime),
			      "ctime not updated after the mapped write: %lld.%09ld -> %lld.%09ld",
			      before.ctime.tv_sec, before.ctime.tv_nsec,
			      after.ctime.tv_sec, after.ctime.tv_nsec);
	KUNIT_EXPECT_EQ_MSG(test, after.size, (loff_t)G215_SIZE,
			    "the file is %lld bytes", after.size);
}

static int g215_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g215_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g215_cases[] = {
	KUNIT_CASE(a_mapped_write_moves_both_times),
	{}
};

static struct kunit_suite g215_suite = {
	.name		= "xfstests/generic/215",
	.suite_init	= g215_suite_init,
	.suite_exit	= g215_suite_exit,
	.test_cases	= g215_cases,
};

kunit_test_suites(&g215_suite);

MODULE_DESCRIPTION("xfstests generic/215 over a loopback NFS mount");
MODULE_LICENSE("GPL");
