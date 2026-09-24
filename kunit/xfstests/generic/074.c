// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/074 over a loopback NFS mount: fstest.
 *
 * Upstream runs src/fstest five times. fstest forks -n children; each
 * child, for -l loops, (re)creates -f files of -s bytes in -b byte blocks
 * and then preads every block back. Block ofs of file fnum in loop loop of
 * child child is filled with (loop + child + fnum + ofs / bs) % 256, so
 * each loop overwrites the previous loop's data with different bytes.
 * -F writes every other block only, leaving holes; -m writes through a
 * shared mapping of the ftruncate'd file instead of pwrite; -S opens the
 * files O_SYNC. The five runs:
 *
 *	0: -l loops
 *	1: -l loops -s 10MiB -b 8192 -m
 *	2: -n children -F -l loops -f files -s 30MiB -b 512
 *	3: -n children -F -l loops -f files -s 30MiB -b 512 -m
 *	4: -n children -F -l loops -f files -s 10MiB -b 512 -mS
 *
 * Each run is a test case here, each child a kthread; fstest's own
 * defaults (1 child, 1 file, 1 MiB, 1024-byte blocks) fill in what a run
 * does not set. A mapped child borrows the test thread's mm with
 * kthread_use_mm() to call vm_mmap() on its file.
 *
 * Deviations: upstream picks loops/files/children by `uname -a | grep
 * SMP`: 10/5/3 on SMP, 2/3/3 otherwise. The KUnit UML kernel is built
 * with CONFIG_SMP=y, so upstream would pick 10/5/3 here. The port uses
 * the 2/3/3 set; the SMP set would need 450 MiB of export for runs 2 and
 * 3, inside a UML kernel booted with mem=1G that also holds the client's
 * page cache. The export is 320 MiB instead of the fixture's 64 MiB default:
 * -F with 512-byte blocks still writes every 4 KiB page, so runs 2 and 3
 * hold three children's three full 30 MiB files, 270 MiB. On a smaller
 * export the WRITEs fail with ENOSPC at writeback, which fstest never
 * sees (it ignores close()), and the check reads zeros.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched/mm.h>

#include "xfstests_nfs_fixture.h"

#define G074_ROOT	XFS_MNT "/g074"
#define G074_EXPORT	"size=335544320,nr_inodes=32768"

/* upstream's "Linux UP" parameter set */
#define G074_LOOPS	2
#define G074_FILES	3
#define G074_CHILDREN	3

#define G074_MIB	(1024 * 1024)

struct g074_cfg {
	int n;			/* the run number, fstest.$n */
	int children;		/* -n */
	int files;		/* -f */
	int size;		/* -s */
	int bs;			/* -b */
	int frags;		/* 2 with -F, else 1 */
	bool mmap;		/* -m */
	bool sync;		/* -S */
};

static const struct g074_cfg g074_cfgs[] = {
	{ 0, 1, 1, G074_MIB, 1024, 1, false, false },
	{ 1, 1, 1, 10 * G074_MIB, 8192, 1, true, false },
	{ 2, G074_CHILDREN, G074_FILES, 30 * G074_MIB, 512, 2, false, false },
	{ 3, G074_CHILDREN, G074_FILES, 30 * G074_MIB, 512, 2, true, false },
	{ 4, G074_CHILDREN, G074_FILES, 10 * G074_MIB, 512, 2, true, true },
};

struct g074_child {
	const struct g074_cfg	*cfg;
	int			child;
	struct mm_struct	*mm;
	u8			*buf;
	int			err;
	char			msg[128];
	struct completion	done;
};

static u8 g074_val(int loop, int child, int fnum, int ofs, int bs)
{
	return (loop + child + fnum + ofs / bs) % 256;
}

static void g074_path(char *p, size_t len, int n, int child, int fnum)
{
	if (fnum < 0)
		snprintf(p, len, G074_ROOT "/fstest.%d/child%d", n, child);
	else
		snprintf(p, len, G074_ROOT "/fstest.%d/child%d/file%d", n, child,
			 fnum);
}

static int g074_fail(struct g074_child *c, int err, const char *what,
		     int fnum, int ofs)
{
	c->err = err;
	snprintf(c->msg, sizeof(c->msg),
		 "child %d file%d offset %d: %s", c->child, fnum, ofs,
		 what);
	return err;
}

/* fstest's create_file() */
static int g074_create_file(struct g074_child *c, int loop, int fnum)
{
	const struct g074_cfg *cfg = c->cfg;
	char path[96];
	struct file *f;
	int size, err = 0;

	g074_path(path, sizeof(path), cfg->n, c->child, fnum);
	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC |
			    (cfg->sync ? O_SYNC : 0), 0644);
	if (IS_ERR(f))
		return g074_fail(c, PTR_ERR(f), "open", fnum, 0);

	if (!cfg->mmap) {
		for (size = 0; size < cfg->size; size += cfg->bs * cfg->frags) {
			loff_t pos = size;

			memset(c->buf, g074_val(loop, c->child, fnum, size,
						cfg->bs), cfg->bs);
			if (kernel_write(f, c->buf, cfg->bs, &pos) != cfg->bs) {
				err = g074_fail(c, -EIO, "write failed", fnum,
						size);
				break;
			}
		}
	} else {
		unsigned long addr;

		err = xfs_ftruncate(f, cfg->size);
		if (err) {
			g074_fail(c, err, "ftruncate", fnum, 0);
			goto out;
		}
		addr = vm_mmap(f, 0, cfg->size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, 0);
		if (IS_ERR_VALUE(addr)) {
			err = g074_fail(c, (int)addr, "mmap", fnum, 0);
			goto out;
		}
		for (size = 0; size < cfg->size; size += cfg->bs * cfg->frags) {
			memset(c->buf, g074_val(loop, c->child, fnum, size,
						cfg->bs), cfg->bs);
			if (copy_to_user((void __user *)(addr + size), c->buf,
					 cfg->bs)) {
				err = g074_fail(c, -EFAULT, "mapped write",
						fnum, size);
				break;
			}
		}
		vm_munmap(addr, cfg->size);
	}
out:
	filp_close(f, NULL);
	return err;
}

/* fstest's check_file() */
static int g074_check_file(struct g074_child *c, int loop, int fnum)
{
	const struct g074_cfg *cfg = c->cfg;
	char path[96];
	struct file *f;
	int size, err = 0;

	g074_path(path, sizeof(path), cfg->n, c->child, fnum);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return g074_fail(c, PTR_ERR(f), "open for check", fnum, 0);

	for (size = 0; size < cfg->size; size += cfg->bs * cfg->frags) {
		loff_t pos = size;
		u8 want = g074_val(loop, c->child, fnum, size, cfg->bs);
		u8 *bad;

		if (kernel_read(f, c->buf, cfg->bs, &pos) != cfg->bs) {
			err = g074_fail(c, -EIO, "read failed", fnum, size);
			break;
		}
		bad = memchr_inv(c->buf, want, cfg->bs);
		if (bad) {
			c->err = -EILSEQ;
			snprintf(c->msg, sizeof(c->msg),
				 "Corruption in child %d fnum %d at offset %d: %02x, expected %02x (loop %d)",
				 c->child, fnum, size + (int)(bad - c->buf),
				 *bad, want, loop);
			err = c->err;
			break;
		}
	}
	filp_close(f, NULL);
	return err;
}

/* fstest's run_child() */
static int g074_run_child(void *arg)
{
	struct g074_child *c = arg;
	const struct g074_cfg *cfg = c->cfg;
	char path[96];
	int loop, i, err;

	kthread_use_mm(c->mm);

	g074_path(path, sizeof(path), cfg->n, c->child, -1);
	err = xfs_mkdir(path);
	if (err) {
		g074_fail(c, err, "mkdir", -1, 0);
		goto out;
	}

	for (loop = 0; loop < G074_LOOPS && !c->err; loop++) {
		for (i = 0; i < cfg->files && !c->err; i++)
			g074_create_file(c, loop, i);
		for (i = 0; i < cfg->files && !c->err; i++)
			g074_check_file(c, loop, i);
	}

	/* cleanup afterwards */
	for (i = 0; i < cfg->files; i++) {
		g074_path(path, sizeof(path), cfg->n, c->child, i);
		xfs_unlink(path);
	}
	g074_path(path, sizeof(path), cfg->n, c->child, -1);
	xfs_rmdir_settled(path);
out:
	kthread_unuse_mm(c->mm);
	complete(&c->done);
	return 0;
}

static void g074_remove_tree(void *unused)
{
	char path[96];
	int n, child, i;

	for (n = 0; n < ARRAY_SIZE(g074_cfgs); n++) {
		for (child = 0; child < G074_CHILDREN; child++) {
			for (i = 0; i < G074_FILES; i++) {
				g074_path(path, sizeof(path), n, child, i);
				xfs_unlink(path);
			}
			g074_path(path, sizeof(path), n, child, -1);
			xfs_rmdir_settled(path);
		}
		snprintf(path, sizeof(path), G074_ROOT "/fstest.%d", n);
		xfs_rmdir_settled(path);
	}
	xfs_rmdir_settled(G074_ROOT);
}

static void g074_run(struct kunit *test, const struct g074_cfg *cfg)
{
	struct g074_child *kids;
	char path[96];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G074_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g074_remove_tree, NULL),
			0);
	snprintf(path, sizeof(path), G074_ROOT "/fstest.%d", cfg->n);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);

	/* gives the test thread the mm the mapped children borrow */
	KUNIT_ASSERT_NE(test,
			kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ,
				      MAP_PRIVATE | MAP_ANONYMOUS, 0), 0UL);

	kids = kunit_kcalloc(test, cfg->children, sizeof(*kids), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, kids);
	for (i = 0; i < cfg->children; i++) {
		kids[i].buf = kunit_kmalloc(test, cfg->bs, GFP_KERNEL);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, kids[i].buf);
		kids[i].cfg = cfg;
		kids[i].child = i;
		kids[i].mm = current->mm;
		init_completion(&kids[i].done);
	}

	for (i = 0; i < cfg->children; i++) {
		struct task_struct *t = kthread_run(g074_run_child, &kids[i],
						    "g074-%d-%d", cfg->n, i);

		if (IS_ERR(t)) {
			kids[i].err = PTR_ERR(t);
			snprintf(kids[i].msg, sizeof(kids[i].msg),
				 "kthread_run");
			complete(&kids[i].done);
		}
	}
	for (i = 0; i < cfg->children; i++)
		wait_for_completion(&kids[i].done);

	for (i = 0; i < cfg->children; i++)
		KUNIT_EXPECT_EQ_MSG(test, kids[i].err, 0, "fstest.%d: %s (%d)",
				    cfg->n, kids[i].msg, kids[i].err);
}

static void fstest_0(struct kunit *test)
{
	g074_run(test, &g074_cfgs[0]);
}

static void fstest_1_mmap(struct kunit *test)
{
	g074_run(test, &g074_cfgs[1]);
}

static void fstest_2_children_holes(struct kunit *test)
{
	g074_run(test, &g074_cfgs[2]);
}

static void fstest_3_children_holes_mmap(struct kunit *test)
{
	g074_run(test, &g074_cfgs[3]);
}

static void fstest_4_children_holes_mmap_sync(struct kunit *test)
{
	g074_run(test, &g074_cfgs[4]);
}

static int g074_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G074_EXPORT);
	return xfstests_nfs_get();
}

static void g074_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g074_cases[] = {
	KUNIT_CASE(fstest_0),
	KUNIT_CASE_SLOW(fstest_1_mmap),
	KUNIT_CASE_SLOW(fstest_2_children_holes),
	KUNIT_CASE_SLOW(fstest_3_children_holes_mmap),
	KUNIT_CASE_SLOW(fstest_4_children_holes_mmap_sync),
	{}
};

static struct kunit_suite g074_suite = {
	.name		= "xfstests/generic/074",
	.suite_init	= g074_suite_init,
	.suite_exit	= g074_suite_exit,
	.test_cases	= g074_cases,
};

kunit_test_suites(&g074_suite);

MODULE_DESCRIPTION("xfstests generic/074 over a loopback NFS mount");
MODULE_LICENSE("GPL");
