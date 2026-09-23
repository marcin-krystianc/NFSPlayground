// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/247 over a loopback NFS mount: a direct overwriter and
 * a mapped writer on the same file.
 *
 * Upstream fills a file, then starts a dd that overwrites the whole thing
 * with O_DIRECT while, in the foreground, xfs_io maps each megabyte in
 * turn -- walking backwards -- and writes through the mapping. Its pass
 * criterion is silence plus a clean dmesg (mixing direct and mapped
 * writes on one inode is expected to warn about nothing).
 *
 * Over NFS the two paths fight over the same pages: nfs_direct_write()
 * invalidates the client's page cache for its range while
 * nfs_vm_page_mkwrite() is dirtying pages in it, and the invalidation
 * must not drop a dirty page that has not been written back. What the
 * port can check beyond "no crash" is that every byte of the result is
 * one of the two patterns -- never a mixture, and never a zero from a
 * page that was dropped mid-flight.
 *
 * Deviations: 4 MiB in 64 KiB steps rather than 512 MiB in 1 MiB steps
 * (the export is a 64 MiB tmpfs), and the direct overwriter is a kthread.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G247_ROOT	XFS_MNT "/g247"
#define G247_FILE	G247_ROOT "/247"

#define G247_SIZE	(4 * 1024 * 1024)
#define G247_STEP	(64 * 1024)
#define G247_DIRECT	0x44		/* what the direct writer writes */
#define G247_MAPPED	0x4d		/* what the mapped writer writes */

struct g247_dio {
	struct file		*f;
	int			err;
	struct completion	done;
};

static int g247_overwrite(void *arg)
{
	struct g247_dio *d = arg;
	loff_t pos = 0;
	u8 *buf;

	buf = kmalloc(G247_STEP, GFP_KERNEL);
	if (!buf) {
		d->err = -ENOMEM;
		complete(&d->done);
		return 0;
	}
	memset(buf, G247_DIRECT, G247_STEP);

	while (pos < G247_SIZE) {
		ssize_t n = xfs_direct_write(d->f, buf, G247_STEP, &pos);

		if (n != G247_STEP) {
			d->err = n < 0 ? (int)n : -EIO;
			break;
		}
	}
	kfree(buf);
	complete(&d->done);
	return 0;
}

static void g247_remove_tree(void *unused)
{
	xfs_unlink(G247_FILE);
	xfs_rmdir_settled(G247_ROOT);
}

static void a_direct_overwriter_and_a_mapped_writer(struct kunit *test)
{
	struct g247_dio d = {};
	struct task_struct *t;
	struct file *f, *df;
	loff_t pos = 0, off;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G247_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g247_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G247_STEP, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, G247_DIRECT, G247_STEP);

	f = filp_open(G247_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	while (pos < G247_SIZE)
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G247_STEP, &pos),
				(ssize_t)G247_STEP);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	df = filp_open(G247_FILE, O_RDWR | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(df), "direct open: %ld",
			       PTR_ERR(df));
	init_completion(&d.done);
	d.f = df;
	t = kthread_run(g247_overwrite, &d, "g247-dio");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* the mapped writer, walking backwards a step at a time */
	memset(buf, G247_MAPPED, G247_STEP);
	for (off = G247_SIZE - G247_STEP; off >= 0; off -= G247_STEP) {
		unsigned long addr = kunit_vm_mmap(test, f, 0, G247_STEP,
						   PROT_READ | PROT_WRITE,
						   MAP_SHARED, off);

		KUNIT_ASSERT_NE_MSG(test, addr, 0UL,
				    "mapping at %lld failed", off);
		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)addr, buf,
						 G247_STEP),
				    0UL, "mapped write at %lld failed", off);
		KUNIT_EXPECT_EQ(test, vm_munmap(addr, G247_STEP), 0);
	}

	wait_for_completion(&d.done);
	KUNIT_EXPECT_EQ_MSG(test, d.err, 0, "the direct writer failed: %d",
			    d.err);

	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(df, NULL);
	filp_close(f, NULL);

	/* every byte is one of the two patterns */
	for (off = 0; off < G247_SIZE; off += G247_STEP) {
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G247_FILE, buf, G247_STEP, off),
				(ssize_t)G247_STEP);
		for (i = 0; i < G247_STEP; i++)
			if (buf[i] != G247_DIRECT && buf[i] != G247_MAPPED) {
				KUNIT_FAIL(test,
					   "byte %lld is %02x, which neither writer wrote",
					   off + i, buf[i]);
				return;
			}
	}
}

static int g247_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g247_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g247_cases[] = {
	KUNIT_CASE_SLOW(a_direct_overwriter_and_a_mapped_writer),
	{}
};

static struct kunit_suite g247_suite = {
	.name		= "xfstests/generic/247",
	.suite_init	= g247_suite_init,
	.suite_exit	= g247_suite_exit,
	.test_cases	= g247_cases,
};

kunit_test_suites(&g247_suite);

MODULE_DESCRIPTION("xfstests generic/247 over a loopback NFS mount");
MODULE_LICENSE("GPL");
