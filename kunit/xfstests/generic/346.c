// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/346 over a loopback NFS mount: a mapped write and an
 * ordinary write into the same pages.
 *
 * src/holetest with -w is generic/340's experiment with one side changed:
 * thread 0 marks its slot in every page with pwrite(2) instead of through
 * the mapping, while thread 1 still goes through the mapping. Every page
 * must end up holding both ids, so the two paths into the same page have
 * to agree.
 *
 * Over NFS they are genuinely different paths: the mapped write dirties
 * the page through nfs_vm_page_mkwrite() and records its range, while the
 * pwrite goes through nfs_write_begin()/nfs_write_end() on the same page.
 * If either one writes back the whole page rather than just its own
 * range, it overwrites the other's slot with what it read earlier -- or
 * with zeroes, since the file starts as a hole.
 *
 * Deviations: 1 MiB and 4 MiB rather than 1, 16 and 256 MiB. The writer
 * is a kthread; it needs no mm, because pwrite is the point of this
 * variant. Its errors are recorded and asserted on by the test thread.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G346_ROOT	XFS_MNT "/g346"
#define G346_FILE	G346_ROOT "/testfile"
#define G346_SERVER	XFS_EXPORT "/g346/testfile"

#define G346_ID0	0x3460000000000001ULL
#define G346_ID1	0x3460000000000002ULL

struct g346_writer {
	struct file		*f;
	unsigned long		pages;
	unsigned long		slot;
	u64			id;
	int			err;
	struct completion	done;
};

static int g346_pwrite_marks(void *arg)
{
	struct g346_writer *w = arg;
	unsigned long i;

	for (i = 0; i < w->pages; i++) {
		loff_t pos = (loff_t)i * PAGE_SIZE + w->slot;
		ssize_t n = kernel_write(w->f, &w->id, sizeof(w->id), &pos);

		if (n != sizeof(w->id)) {
			w->err = n < 0 ? (int)n : -EIO;
			break;
		}
	}
	complete(&w->done);
	return 0;
}

static void g346_remove_tree(void *unused)
{
	xfs_unlink(G346_FILE);
	xfs_rmdir_settled(G346_ROOT);
}

static void g346_one_size(struct kunit *test, loff_t size)
{
	unsigned long pages = size / PAGE_SIZE;
	unsigned long slot0 = PAGE_SIZE / 4;
	unsigned long slot1 = PAGE_SIZE / 4 + PAGE_SIZE / 2;
	struct g346_writer w = {
		.slot = slot0,
		.id = G346_ID0,
		.pages = pages,
	};
	struct task_struct *t;
	unsigned long addr, i;
	struct file *f;
	u8 *page;
	u64 v;

	xfs_unlink(G346_FILE);
	f = filp_open(G346_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, size), 0);

	addr = kunit_vm_mmap(test, f, 0, size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mapping %lld bytes failed",
			    size);

	init_completion(&w.done);
	w.f = f;
	t = kthread_run(g346_pwrite_marks, &w, "g346-writer");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	for (i = 0; i < pages; i++) {
		u64 id = G346_ID1;

		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)(addr +
						 i * PAGE_SIZE + slot1),
						 &id, sizeof(id)),
				    0UL, "marking page %lu failed", i);
	}

	wait_for_completion(&w.done);
	KUNIT_ASSERT_EQ_MSG(test, w.err, 0, "the pwrite marker failed: %d",
			    w.err);

	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot0),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G346_ID0,
				    "%lld-byte file: page %lu, the pwrite slot is %llx",
				    size, i, v);
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot1),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G346_ID1,
				    "%lld-byte file: page %lu, the mapped slot is %llx",
				    size, i, v);
	}

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, size), 0);
	filp_close(f, NULL);

	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G346_SERVER, page, PAGE_SIZE,
					       (loff_t)i * PAGE_SIZE),
				(ssize_t)PAGE_SIZE);
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot0), G346_ID0,
				    "server: page %lu, the pwrite slot is %llx",
				    i, *(u64 *)(page + slot0));
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot1), G346_ID1,
				    "server: page %lu, the mapped slot is %llx",
				    i, *(u64 *)(page + slot1));
	}
}

static void a_mapped_write_and_a_pwrite_share_every_page(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G346_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g346_remove_tree, NULL),
			0);

	g346_one_size(test, 1024 * 1024);
	g346_one_size(test, 4 * 1024 * 1024);
}

static int g346_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g346_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g346_cases[] = {
	KUNIT_CASE_SLOW(a_mapped_write_and_a_pwrite_share_every_page),
	{}
};

static struct kunit_suite g346_suite = {
	.name		= "xfstests/generic/346",
	.suite_init	= g346_suite_init,
	.suite_exit	= g346_suite_exit,
	.test_cases	= g346_cases,
};

kunit_test_suites(&g346_suite);

MODULE_DESCRIPTION("xfstests generic/346 over a loopback NFS mount");
MODULE_LICENSE("GPL");
