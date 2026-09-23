// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/340 over a loopback NFS mount: two threads writing
 * into different halves of the same pages through one shared mapping.
 *
 * src/holetest creates a sparse file, maps it MAP_SHARED, and starts two
 * threads. Each thread writes its own id into its own eight-byte slot in
 * every page -- thread 0 a quarter of the way in, thread 1 three quarters
 * -- and afterwards every page must hold both ids. The file starts as a
 * hole, so each page is faulted in for the first time by whichever thread
 * gets there first; if the fault path loses the other thread's write, one
 * of the two slots reads back as zero.
 *
 * Over NFS that first fault is nfs_vm_page_mkwrite() on a page with no
 * server data behind it, and the two writes then have to end up in the
 * same dirty range rather than as two competing versions of the page. The
 * port checks both slots of every page, as upstream does, and then checks
 * the file on the server after the mapping is torn down -- upstream only
 * looks at the mapping.
 *
 * Deviations: 1 MiB and 4 MiB rather than upstream's 1 MiB, 16 MiB and
 * 256 MiB, because the export is a 64 MiB tmpfs. The second thread is a
 * kthread that borrows the test thread's mm with kthread_use_mm(), which
 * is what makes the mapping visible to both, and it records its errors
 * rather than asserting -- KUnit assertions only work in the test thread.
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
#include <linux/sched/mm.h>

#include "xfstests_nfs_fixture.h"

#define G340_ROOT	XFS_MNT "/g340"
#define G340_FILE	G340_ROOT "/testfile"
#define G340_SERVER	XFS_EXPORT "/g340/testfile"

#define G340_ID0	0x3400000000000001ULL
#define G340_ID1	0x3400000000000002ULL

struct g340_marker {
	struct mm_struct	*mm;
	unsigned long		addr;
	unsigned long		pages;
	unsigned long		slot;		/* offset within each page */
	u64			id;
	int			err;
	struct completion	done;
};

static int g340_mark(void *arg)
{
	struct g340_marker *m = arg;
	unsigned long i;

	kthread_use_mm(m->mm);
	for (i = 0; i < m->pages; i++) {
		unsigned long at = m->addr + i * PAGE_SIZE + m->slot;

		if (copy_to_user((void __user *)at, &m->id, sizeof(m->id))) {
			m->err = -EFAULT;
			break;
		}
	}
	kthread_unuse_mm(m->mm);
	complete(&m->done);
	return 0;
}

static void g340_remove_tree(void *unused)
{
	xfs_unlink(G340_FILE);
	xfs_rmdir_settled(G340_ROOT);
}

static void g340_one_size(struct kunit *test, loff_t size)
{
	unsigned long pages = size / PAGE_SIZE;
	unsigned long slot0 = PAGE_SIZE / 4;
	unsigned long slot1 = PAGE_SIZE / 4 + PAGE_SIZE / 2;
	struct g340_marker m = {
		.slot = slot0,
		.id = G340_ID0,
	};
	struct task_struct *t;
	unsigned long addr, i;
	struct file *f;
	u64 v;
	u8 *page;

	xfs_unlink(G340_FILE);
	f = filp_open(G340_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, size), 0);

	addr = kunit_vm_mmap(test, f, 0, size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "mapping %lld bytes failed",
			    size);

	init_completion(&m.done);
	m.mm = current->mm;
	m.addr = addr;
	m.pages = pages;

	t = kthread_run(g340_mark, &m, "g340-marker");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* this thread is upstream's second marker */
	for (i = 0; i < pages; i++) {
		u64 id = G340_ID1;

		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)(addr +
						 i * PAGE_SIZE + slot1),
						 &id, sizeof(id)),
				    0UL, "marking page %lu failed", i);
	}

	wait_for_completion(&m.done);
	KUNIT_ASSERT_EQ_MSG(test, m.err, 0, "the other marker failed: %d",
			    m.err);

	/* every page holds both ids */
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot0),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G340_ID0,
				    "%lld-byte file: page %lu slot 0 is %llx",
				    size, i, v);
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot1),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G340_ID1,
				    "%lld-byte file: page %lu slot 1 is %llx",
				    size, i, v);
	}

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, size), 0);
	filp_close(f, NULL);

	/* and the same bytes reached the server */
	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G340_SERVER, page, PAGE_SIZE,
					       (loff_t)i * PAGE_SIZE),
				(ssize_t)PAGE_SIZE);
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot0), G340_ID0,
				    "server: page %lu slot 0 is %llx", i,
				    *(u64 *)(page + slot0));
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot1), G340_ID1,
				    "server: page %lu slot 1 is %llx", i,
				    *(u64 *)(page + slot1));
	}
}

static void two_markers_share_every_page(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G340_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g340_remove_tree, NULL),
			0);

	g340_one_size(test, 1024 * 1024);
	g340_one_size(test, 4 * 1024 * 1024);
}

static int g340_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g340_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g340_cases[] = {
	KUNIT_CASE_SLOW(two_markers_share_every_page),
	{}
};

static struct kunit_suite g340_suite = {
	.name		= "xfstests/generic/340",
	.suite_init	= g340_suite_init,
	.suite_exit	= g340_suite_exit,
	.test_cases	= g340_cases,
};

kunit_test_suites(&g340_suite);

MODULE_DESCRIPTION("xfstests generic/340 over a loopback NFS mount");
MODULE_LICENSE("GPL");
