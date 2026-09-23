// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/344 over a loopback NFS mount: the same races as
 * generic/340 and generic/346, but with the pages already faulted in.
 *
 * src/holetest with -r reads every page of the mapping before the two
 * markers start ("prefault"), so the race is no longer about who faults a
 * hole in first: the pages are present and clean, and the two writers
 * have to dirty them without losing each other's slot. Upstream runs it
 * both ways, with both markers going through the mapping (-r) and with
 * one of them using pwrite (-r -w).
 *
 * Over NFS a prefaulted page is one the client has already read from the
 * server (or zero-filled for a hole), so the write path takes the
 * "partial write to an up-to-date page" branch rather than the
 * "brand-new page" one. That is a different path through
 * nfs_write_begin(), and it is the one this test reaches.
 *
 * Deviations: 4 MiB rather than upstream's 16 and 256 MiB; the second
 * marker is a kthread, borrowing the test thread's mm where it writes
 * through the mapping. The prefault step also asserts the pages read as
 * zero, which is upstream's prefault_mapping() check.
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

#define G344_ROOT	XFS_MNT "/g344"
#define G344_FILE	G344_ROOT "/testfile"
#define G344_SIZE	(4 * 1024 * 1024)

#define G344_ID0	0x3440000000000001ULL
#define G344_ID1	0x3440000000000002ULL

struct g344_marker {
	struct mm_struct	*mm;		/* NULL: use pwrite instead */
	struct file		*f;
	unsigned long		addr;
	unsigned long		pages;
	unsigned long		slot;
	u64			id;
	int			err;
	struct completion	done;
};

static int g344_mark(void *arg)
{
	struct g344_marker *m = arg;
	unsigned long i;

	if (m->mm)
		kthread_use_mm(m->mm);
	for (i = 0; i < m->pages; i++) {
		if (m->mm) {
			unsigned long at = m->addr + i * PAGE_SIZE + m->slot;

			if (copy_to_user((void __user *)at, &m->id,
					 sizeof(m->id))) {
				m->err = -EFAULT;
				break;
			}
		} else {
			loff_t pos = (loff_t)i * PAGE_SIZE + m->slot;
			ssize_t n = kernel_write(m->f, &m->id, sizeof(m->id),
						 &pos);

			if (n != sizeof(m->id)) {
				m->err = n < 0 ? (int)n : -EIO;
				break;
			}
		}
	}
	if (m->mm)
		kthread_unuse_mm(m->mm);
	complete(&m->done);
	return 0;
}

static void g344_remove_tree(void *unused)
{
	xfs_unlink(G344_FILE);
	xfs_rmdir_settled(G344_ROOT);
}

static void g344_round(struct kunit *test, bool other_uses_mmap)
{
	unsigned long pages = G344_SIZE / PAGE_SIZE;
	unsigned long slot0 = PAGE_SIZE / 4;
	unsigned long slot1 = PAGE_SIZE / 4 + PAGE_SIZE / 2;
	const char *what = other_uses_mmap ? "both mapped" : "one pwrite";
	struct g344_marker m = { .slot = slot0, .id = G344_ID0 };
	struct task_struct *t;
	unsigned long addr, i;
	struct file *f;
	u64 v;

	xfs_unlink(G344_FILE);
	f = filp_open(G344_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
			       PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G344_SIZE), 0);

	addr = kunit_vm_mmap(test, f, 0, G344_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "%s: mmap failed", what);

	/* upstream's prefault_mapping(): touch every page, expect zeroes */
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE), sizeof(v)),
				0UL);
		KUNIT_ASSERT_EQ_MSG(test, v, 0ULL,
				    "%s: page %lu was not zero before the test",
				    what, i);
	}

	init_completion(&m.done);
	m.pages = pages;
	m.addr = addr;
	m.f = f;
	m.mm = other_uses_mmap ? current->mm : NULL;

	t = kthread_run(g344_mark, &m, "g344-marker");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "%s: kthread_run: %ld", what,
			       PTR_ERR(t));

	for (i = 0; i < pages; i++) {
		u64 id = G344_ID1;

		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)(addr +
						 i * PAGE_SIZE + slot1),
						 &id, sizeof(id)),
				    0UL, "%s: marking page %lu failed", what,
				    i);
	}

	wait_for_completion(&m.done);
	KUNIT_ASSERT_EQ_MSG(test, m.err, 0, "%s: the other marker failed: %d",
			    what, m.err);

	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot0),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G344_ID0,
				    "%s: page %lu slot 0 is %llx", what, i, v);
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot1),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G344_ID1,
				    "%s: page %lu slot 1 is %llx", what, i, v);
	}

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G344_SIZE), 0);
	filp_close(f, NULL);
}

static void prefaulted_pages_keep_both_marks(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G344_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g344_remove_tree, NULL),
			0);

	g344_round(test, true);		/* holetest -f -r */
	g344_round(test, false);	/* holetest -f -r -w */
}

static int g344_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g344_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g344_cases[] = {
	KUNIT_CASE_SLOW(prefaulted_pages_keep_both_marks),
	{}
};

static struct kunit_suite g344_suite = {
	.name		= "xfstests/generic/344",
	.suite_init	= g344_suite_init,
	.suite_exit	= g344_suite_exit,
	.test_cases	= g344_cases,
};

kunit_test_suites(&g344_suite);

MODULE_DESCRIPTION("xfstests generic/344 over a loopback NFS mount");
MODULE_LICENSE("GPL");
