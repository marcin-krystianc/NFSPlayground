// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/567 over a loopback NFS mount: a mapped write after a
 * punch hole in the same range.
 *
 * Upstream fills 12K with 0x58, maps it, writes 0x5a through the mapping
 * over 2K..10K, punches 2K..10K, writes 0x59 through the mapping over the
 * same range, and closes. The result must be 0x58, 0x59, 0x58 both before
 * and after a mount cycle: the second mapped write must reach the file
 * even though the punch removed the pages it faults back in.
 *
 * Over NFS the punch is nfs42_proc_deallocate(), which drops the range
 * with truncate_pagecache_range() after the DEALLOCATE RPC. The second
 * mapped write then faults the pages back through nfs_vm_page_mkwrite(),
 * and close flushes them.
 *
 * Deviations: the post-remount hexdump is a read of the file through the
 * tmpfs export (the server's own bytes), as in the generic/029 port, and
 * the golden hexdump is a byte model.
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

#include "xfstests_nfs_fixture.h"

#define G567_ROOT	XFS_MNT "/g567"
#define G567_FILE	G567_ROOT "/testfile"
#define G567_SERVER	XFS_EXPORT "/g567/testfile"

#define G567_SIZE	12288
#define G567_OFF	2048
#define G567_LEN	8192

static void g567_remove_tree(void *unused)
{
	xfs_unlink(G567_FILE);
	xfs_rmdir_settled(G567_ROOT);
}

static void g567_verify(struct kunit *test, const char *path,
			const u8 *want, const char *which)
{
	u8 *got;
	int i;

	got = kunit_kmalloc(test, G567_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	KUNIT_ASSERT_EQ_MSG(test, xfs_read_range(path, got, G567_SIZE, 0),
			    (ssize_t)G567_SIZE, "%s: short read", which);
	for (i = 0; i < G567_SIZE; i++)
		if (got[i] != want[i]) {
			KUNIT_FAIL(test, "%s: byte %d is %02x, expected %02x",
				   which, i, got[i], want[i]);
			return;
		}
}

static void mapped_write_after_punch_reaches_the_file(struct kunit *test)
{
	unsigned long addr;
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf, *want;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G567_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g567_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G567_SIZE, GFP_KERNEL);
	want = kunit_kmalloc(test, G567_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, want);

	/* xfs_io -t -f -c "pwrite -S 0x58 0 12288" */
	f = filp_open(G567_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	memset(buf, 0x58, G567_SIZE);
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G567_SIZE, &pos),
			(ssize_t)G567_SIZE);

	/* -c "mmap -rw 0 12288" */
	addr = kunit_vm_mmap(test, f, 0, G567_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mmap failed");

	/* -c "mwrite -S 0x5a 2048 8192" */
	memset(buf, 0x5a, G567_LEN);
	KUNIT_ASSERT_EQ(test,
			copy_to_user((void __user *)(addr + G567_OFF), buf,
				     G567_LEN), 0UL);

	/* -c "fpunch 2048 8192" */
	KUNIT_ASSERT_EQ(test,
			vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
				      FALLOC_FL_KEEP_SIZE, G567_OFF, G567_LEN),
			0);

	/* -c "mwrite -S 0x59 2048 8192" */
	memset(buf, 0x59, G567_LEN);
	KUNIT_ASSERT_EQ(test,
			copy_to_user((void __user *)(addr + G567_OFF), buf,
				     G567_LEN), 0UL);

	/* -c "close" */
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G567_SIZE), 0);
	filp_close(f, NULL);

	memset(want, 0x58, G567_SIZE);
	memset(want + G567_OFF, 0x59, G567_LEN);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G567_FILE, &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)G567_SIZE);
	/* "Pre-Remount" */
	g567_verify(test, G567_FILE, want, "client");
	/* "Post-Remount": the server's own bytes */
	g567_verify(test, G567_SERVER, want, "SERVER");
}

static int g567_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g567_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g567_cases[] = {
	KUNIT_CASE(mapped_write_after_punch_reaches_the_file),
	{}
};

static struct kunit_suite g567_suite = {
	.name		= "xfstests/generic/567",
	.suite_init	= g567_suite_init,
	.suite_exit	= g567_suite_exit,
	.test_cases	= g567_cases,
};

kunit_test_suites(&g567_suite);

MODULE_DESCRIPTION("xfstests generic/567 over a loopback NFS mount");
MODULE_LICENSE("GPL");
