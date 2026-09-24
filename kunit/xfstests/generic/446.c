// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/446 over a loopback NFS mount: direct reads racing
 * mapped writes and punch hole on the same range.
 *
 * Upstream truncates a file to 256K, then runs 1000 O_DIRECT reads of the
 * first 128K in the background while the foreground loops: map the first
 * 128K, write it through the mapping, close, punch it out. The pass
 * criterion is "Silence is golden" plus a clean dmesg -- no warning or
 * oops from the direct-I/O path meeting pages that mmap dirtied and punch
 * removed.
 *
 * Over NFS the reads are nfs_file_direct_read(), the mapped writes fault
 * through nfs_vm_page_mkwrite() and are flushed on close
 * (nfs4_file_flush()), and the punch is nfs42_proc_deallocate(), which
 * blocks O_DIRECT (nfs_file_block_o_direct()) and drops the range from
 * the page cache after the DEALLOCATE RPC.
 *
 * Deviations: the reader is a kthread sharing one O_DIRECT open file
 * rather than a process re-opening it per read, as in the generic/391
 * port. The dmesg check has no KUnit equivalent: the port requires every
 * read, mapped write and punch to succeed, and a WARN from the race shows
 * only in the run's kernel log, not as a test failure.
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
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G446_ROOT	XFS_MNT "/g446"
#define G446_FILE	G446_ROOT "/file"

#define G446_FILESZ	(65536 * 2)
#define G446_READS	1000

struct g446_reader {
	struct file		*f;
	int			err;
	struct completion	done;
};

static int g446_dread(void *arg)
{
	struct g446_reader *r = arg;
	u8 *buf;
	int i;

	buf = kmalloc(G446_FILESZ, GFP_KERNEL);
	if (!buf) {
		r->err = -ENOMEM;
		complete(&r->done);
		return 0;
	}
	for (i = 0; i < G446_READS; i++) {
		loff_t pos = 0;
		ssize_t n = xfs_direct_read(r->f, buf, G446_FILESZ, &pos);

		if (n != G446_FILESZ) {
			r->err = n < 0 ? (int)n : -EIO;
			break;
		}
	}
	kfree(buf);
	complete(&r->done);
	return 0;
}

static void g446_remove_tree(void *unused)
{
	xfs_unlink(G446_FILE);
	xfs_rmdir_settled(G446_ROOT);
}

static void direct_reads_race_mmap_write_and_punch(struct kunit *test)
{
	struct g446_reader r = {};
	struct task_struct *t;
	unsigned long iters = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G446_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g446_remove_tree, NULL),
			0);

	/* xfs_io -f -c "truncate $((filesz * 2))" */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G446_FILE, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_truncate(G446_FILE, G446_FILESZ * 2), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, PAGE_SIZE);

	r.f = filp_open(G446_FILE, O_RDONLY | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(r.f), "direct open: %ld",
			       PTR_ERR(r.f));
	init_completion(&r.done);
	t = kthread_run(g446_dread, &r, "g446-dread");
	if (IS_ERR(t)) {
		filp_close(r.f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "kthread_run: %ld", PTR_ERR(t));
	}

	while (!completion_done(&r.done)) {
		unsigned long addr, off;
		struct file *f;
		int err;

		/* xfs_io -c "mmap 0 $filesz" -c "mwrite 0 $filesz" */
		f = filp_open(G446_FILE, O_RDWR, 0);
		if (IS_ERR(f)) {
			KUNIT_FAIL(test, "iteration %lu: open: %ld", iters,
				   PTR_ERR(f));
			break;
		}
		addr = kunit_vm_mmap(test, f, 0, G446_FILESZ,
				     PROT_READ | PROT_WRITE, MAP_SHARED, 0);
		if (!addr) {
			filp_close(f, NULL);
			KUNIT_FAIL(test, "iteration %lu: mmap failed", iters);
			break;
		}
		for (off = 0; off < G446_FILESZ; off += PAGE_SIZE)
			if (copy_to_user((void __user *)(addr + off), buf,
					 PAGE_SIZE))
				break;
		vm_munmap(addr, G446_FILESZ);
		err = filp_close(f, NULL);
		if (off < G446_FILESZ || err) {
			KUNIT_FAIL(test,
				   "iteration %lu: mwrite stopped at %lu, close %d",
				   iters, off, err);
			break;
		}

		/* xfs_io -c "fpunch 0 $filesz" */
		f = filp_open(G446_FILE, O_RDWR, 0);
		if (IS_ERR(f)) {
			KUNIT_FAIL(test, "iteration %lu: open: %ld", iters,
				   PTR_ERR(f));
			break;
		}
		err = vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
				    FALLOC_FL_KEEP_SIZE, 0, G446_FILESZ);
		filp_close(f, NULL);
		if (err) {
			KUNIT_FAIL(test, "iteration %lu: fpunch: %d", iters,
				   err);
			break;
		}
		iters++;
	}

	wait_for_completion(&r.done);
	filp_close(r.f, NULL);
	KUNIT_EXPECT_EQ_MSG(test, r.err, 0, "direct reader failed: %d", r.err);
	kunit_info(test, "%lu mwrite+fpunch iterations during %d direct reads\n",
		   iters, G446_READS);
}

static int g446_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g446_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g446_cases[] = {
	KUNIT_CASE_SLOW(direct_reads_race_mmap_write_and_punch),
	{}
};

static struct kunit_suite g446_suite = {
	.name		= "xfstests/generic/446",
	.suite_init	= g446_suite_init,
	.suite_exit	= g446_suite_exit,
	.test_cases	= g446_cases,
};

kunit_test_suites(&g446_suite);

MODULE_DESCRIPTION("xfstests generic/446 over a loopback NFS mount");
MODULE_LICENSE("GPL");
