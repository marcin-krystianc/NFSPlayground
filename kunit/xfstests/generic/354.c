// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/354 over a loopback NFS mount: two markers in one
 * MAP_PRIVATE mapping.
 *
 * src/holetest with -p makes the mapping private instead of shared, so
 * the two markers' writes land in copy-on-write pages: both must still be
 * visible through the mapping, and none of them may reach the file.
 * Upstream runs it with threads (-p) and with processes (-p -F), where
 * each process gets its own copy and sees only its own mark.
 *
 * Over NFS the COW fault has to read the page from the server (or
 * zero-fill it for a hole) and then detach it from the file's mapping, so
 * what this checks is that a private fault does not dirty the file's own
 * page or send a WRITE. The port covers the thread variant; the process
 * variant needs two address spaces, and in a kernel test both markers are
 * kthreads borrowing one mm.
 *
 * Deviations: 4 MiB rather than 16 and 256 MiB. The file is checked
 * afterwards through the server's own copy, which is a stronger version
 * of upstream's "map it again read-only and expect zeroes".
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

#define G354_ROOT	XFS_MNT "/g354"
#define G354_FILE	G354_ROOT "/testfile"
#define G354_SERVER	XFS_EXPORT "/g354/testfile"
#define G354_SIZE	(4 * 1024 * 1024)

#define G354_ID0	0x3540000000000001ULL
#define G354_ID1	0x3540000000000002ULL

struct g354_marker {
	struct mm_struct	*mm;
	unsigned long		addr;
	unsigned long		pages;
	unsigned long		slot;
	u64			id;
	int			err;
	struct completion	done;
};

static int g354_mark(void *arg)
{
	struct g354_marker *m = arg;
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

static void g354_remove_tree(void *unused)
{
	xfs_unlink(G354_FILE);
	xfs_rmdir_settled(G354_ROOT);
}

static void a_private_mapping_keeps_its_writes_to_itself(struct kunit *test)
{
	unsigned long pages = G354_SIZE / PAGE_SIZE;
	unsigned long slot0 = PAGE_SIZE / 4;
	unsigned long slot1 = PAGE_SIZE / 4 + PAGE_SIZE / 2;
	struct g354_marker m = { .slot = slot0, .id = G354_ID0 };
	struct task_struct *t;
	unsigned long addr, i;
	struct file *f;
	u8 *page;
	u64 v;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G354_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g354_remove_tree, NULL),
			0);

	f = filp_open(G354_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G354_SIZE), 0);

	addr = kunit_vm_mmap(test, f, 0, G354_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "private mapping failed");

	init_completion(&m.done);
	m.mm = current->mm;
	m.addr = addr;
	m.pages = pages;

	t = kthread_run(g354_mark, &m, "g354-marker");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	for (i = 0; i < pages; i++) {
		u64 id = G354_ID1;

		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)(addr +
						 i * PAGE_SIZE + slot1),
						 &id, sizeof(id)),
				    0UL, "marking page %lu failed", i);
	}

	wait_for_completion(&m.done);
	KUNIT_ASSERT_EQ_MSG(test, m.err, 0, "the other marker failed: %d",
			    m.err);

	/* both marks are visible in the private mapping */
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot0),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G354_ID0,
				    "page %lu slot 0 is %llx", i, v);
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&v, (void __user *)(addr +
						i * PAGE_SIZE + slot1),
					       sizeof(v)), 0UL);
		KUNIT_EXPECT_EQ_MSG(test, v, G354_ID1,
				    "page %lu slot 1 is %llx", i, v);
	}

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G354_SIZE), 0);
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	/* and none of them reached the file */
	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	for (i = 0; i < pages; i++) {
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G354_SERVER, page, PAGE_SIZE,
					       (loff_t)i * PAGE_SIZE),
				(ssize_t)PAGE_SIZE);
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot0), 0ULL,
				    "the private write reached the server: page %lu slot 0 is %llx",
				    i, *(u64 *)(page + slot0));
		KUNIT_EXPECT_EQ_MSG(test, *(u64 *)(page + slot1), 0ULL,
				    "the private write reached the server: page %lu slot 1 is %llx",
				    i, *(u64 *)(page + slot1));
	}
}

static int g354_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g354_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g354_cases[] = {
	KUNIT_CASE_SLOW(a_private_mapping_keeps_its_writes_to_itself),
	{}
};

static struct kunit_suite g354_suite = {
	.name		= "xfstests/generic/354",
	.suite_init	= g354_suite_init,
	.suite_exit	= g354_suite_exit,
	.test_cases	= g354_cases,
};

kunit_test_suites(&g354_suite);

MODULE_DESCRIPTION("xfstests generic/354 over a loopback NFS mount");
MODULE_LICENSE("GPL");
