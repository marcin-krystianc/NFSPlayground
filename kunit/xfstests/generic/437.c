// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/437 over a loopback NFS mount: copy-on-write faults on
 * a private file mapping racing MADV_DONTNEED.
 *
 * src/t_mmap_cow_race writes one byte through a 2 MiB shared mapping,
 * then 500 times maps the file MAP_PRIVATE and starts two threads. Each
 * thread, ten times: reads the first byte, writes it back (a COW fault
 * that copies the file's page into a private anonymous page), and drops
 * the range with MADV_DONTNEED, sleeping up to 100 us in between. The DAX
 * bug it was written for crashed in the COW path; the pass criterion is
 * that the program finishes.
 *
 * Over NFS the COW source is a page-cache folio that nfs_read_folio()
 * filled, and MADV_DONTNEED on the private mapping drops only the private
 * copy, so the next read faults the file's page back.
 *
 * Deviations: the second thread is a kthread borrowing the test thread's
 * mm, as in the generic/340 port. The workers touch the byte with
 * access_remote_vm() (GUP under the mmap lock), not copy_to_user(): on
 * UML, copy_to_user() racing the other worker's MADV_DONTNEED panicked
 * the kernel with a NULL dereference in maybe_map()
 * (arch/um/kernel/skas/uaccess.c), which re-walks the page table after
 * handle_page_fault() and dereferences the result unchecked. GUP takes
 * the same COW fault. The port adds a check upstream does not make:
 * every read of the first byte must return 1, the value the shared write
 * put in the file, since each thread only writes back what it read.
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
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/sched/mm.h>

#include "xfstests_nfs_fixture.h"

#define G437_ROOT	XFS_MNT "/g437"
#define G437_FILE	G437_ROOT "/testfile"
#define G437_SERVER	XFS_EXPORT "/g437/testfile"

#define G437_LEN	(2 * 1024 * 1024)
#define G437_MAPS	500
#define G437_LOOPS	10

struct g437_worker {
	struct mm_struct	*mm;
	unsigned long		addr;
	int			err;
	int			bad;	/* reads that were not 1 */
	struct completion	done;
};

/*
 * upstream's worker_fn. The byte is read and written with
 * access_remote_vm() rather than copy_{from,to}_user(): see the header.
 */
static void g437_work(struct g437_worker *w)
{
	int i;

	for (i = 0; i < G437_LOOPS; i++) {
		u8 a;

		if (access_remote_vm(w->mm, w->addr, &a, 1, 0) != 1 ||
		    access_remote_vm(w->mm, w->addr, &a, 1, FOLL_WRITE) != 1) {
			w->err = -EFAULT;
			return;
		}
		if (a != 1)
			w->bad++;
		w->err = do_madvise(w->mm, w->addr, G437_LEN, MADV_DONTNEED);
		if (w->err)
			return;
		usleep_range(0, get_random_u32_below(100) + 1);
	}
}

static int g437_kthread(void *arg)
{
	struct g437_worker *w = arg;

	kthread_use_mm(w->mm);
	g437_work(w);
	kthread_unuse_mm(w->mm);
	complete(&w->done);
	return 0;
}

static void g437_remove_tree(void *unused)
{
	xfs_unlink(G437_FILE);
	xfs_rmdir_settled(G437_ROOT);
}

static void private_cow_races_madv_dontneed(struct kunit *test)
{
	unsigned long addr;
	struct file *f;
	u8 one = 1;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G437_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g437_remove_tree, NULL),
			0);

	f = filp_open(G437_FILE, O_RDWR | O_CREAT, 0600);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G437_LEN), 0);

	/* data[0] = 1 through a shared mapping */
	addr = kunit_vm_mmap(test, f, 0, G437_LEN, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "shared mmap failed");
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, &one, 1), 0UL);
	KUNIT_ASSERT_EQ(test, vm_munmap(addr, G437_LEN), 0);

	for (i = 0; i < G437_MAPS; i++) {
		struct g437_worker w0 = {}, w1 = {};
		struct task_struct *t;

		addr = kunit_vm_mmap(test, f, 0, G437_LEN,
				     PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
		KUNIT_ASSERT_NE_MSG(test, addr, 0UL,
				    "private mmap %d failed", i);

		w0.mm = w1.mm = current->mm;
		w0.addr = w1.addr = addr;
		init_completion(&w0.done);
		t = kthread_run(g437_kthread, &w0, "g437-worker");
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
		g437_work(&w1);
		wait_for_completion(&w0.done);

		KUNIT_ASSERT_EQ(test, vm_munmap(addr, G437_LEN), 0);
		KUNIT_ASSERT_EQ_MSG(test, w0.err, 0, "map %d: worker 0: %d", i,
				    w0.err);
		KUNIT_ASSERT_EQ_MSG(test, w1.err, 0, "map %d: worker 1: %d", i,
				    w1.err);
		KUNIT_ASSERT_EQ_MSG(test, w0.bad + w1.bad, 0,
				    "map %d: %d reads of the first byte were not 1",
				    i, w0.bad + w1.bad);
	}
	filp_close(f, NULL);

	/* the shared write reached the server */
	KUNIT_ASSERT_EQ(test, xfs_read_range(G437_SERVER, &one, 1, 0),
			(ssize_t)1);
	KUNIT_EXPECT_EQ(test, one, 1);
}

static int g437_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g437_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g437_cases[] = {
	KUNIT_CASE_SLOW(private_cow_races_madv_dontneed),
	{}
};

static struct kunit_suite g437_suite = {
	.name		= "xfstests/generic/437",
	.suite_init	= g437_suite_init,
	.suite_exit	= g437_suite_exit,
	.test_cases	= g437_cases,
};

kunit_test_suites(&g437_suite);

MODULE_DESCRIPTION("xfstests generic/437 over a loopback NFS mount");
MODULE_LICENSE("GPL");
