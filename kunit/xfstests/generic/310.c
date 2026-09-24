// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/310 over a loopback NFS mount: read(2) or lseek(2)
 * racing readdir on the same directory descriptor.
 *
 * Upstream fills a directory with 4096 files and runs two programs
 * against it, each for RUN_TIME (30 s) before killing it:
 *
 *	t_readdir_1: opendir, fork; the child read(2)s the directory fd in
 *	    a loop, the parent lseeks it to 0 and readdir(3)s to the end, in
 *	    a loop
 *	t_readdir_2: the same, but the child lseeks the shared fd to 0, 2,
 *	    1023, 1025, 4096 and 0x7fffffff (its loop steps i twice), then
 *	    lseek(SEEK_CUR) and back, in a loop
 *
 * A fork shares the open file, so both sides move one f_pos. On ext3 the
 * pair produced "bad entry in directory" errors; the pass criterion is
 * that dmesg gains no BUG, NULL dereference, WARNING or lockdep report.
 *
 * Over NFS the shared position is the client's readdir cookie and the
 * walk is served from its readdir page cache, so a read(2) that disturbed
 * f_pos, or an lseek to a position the server never issued, lands in the
 * cookie handling.
 *
 * Each system call is reproduced with the lock the syscall takes: read(2),
 * lseek(2) and getdents64(2) all hold the file's f_pos_lock around their
 * work on a directory (fdget_pos). readdir(3) is getdents64 into glibc's
 * buffer, sized by xfs_libc_dirbuf(). The child is a kthread; the read(2) side borrows this
 * thread's mm for its user buffer, since kernel_read() refuses a file with
 * a ->read method.
 *
 * Checks beyond upstream's: every read(2) of the directory must fail with
 * EISDIR, and in t_readdir_1, where nothing but the parent moves the
 * position, every walk must return all 4098 entries.
 *
 * Deviation: each program runs for 5 s rather than 30 s.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched/mm.h>
#include <linux/jiffies.h>

#include "xfstests_nfs_fixture.h"

#define G310_ROOT	XFS_MNT "/g310"
#define G310_DIR	G310_ROOT "/310"
#define G310_ENTRIES	4096
#define G310_RUN_MS	5000
/* the whole directory: no getdents64 call can return more */
#define G310_BATCH	(G310_ENTRIES + 2)

struct g310_child {
	struct file		*d;
	struct mm_struct	*mm;
	unsigned long		buf;	/* user address for read(2) */
	atomic_t		stop;
	unsigned long		calls;
	int			wrong;	/* reads that did not fail EISDIR */
	int			lseek_err;
	struct completion	done;
};

/* t_readdir_1's child: while (1) read(fd, buf, 100); */
static int g310_read_loop(void *arg)
{
	struct g310_child *c = arg;

	kthread_use_mm(c->mm);
	while (!atomic_read(&c->stop)) {
		ssize_t n;

		mutex_lock(&c->d->f_pos_lock);
		n = vfs_read(c->d, (char __user *)c->buf, 100, &c->d->f_pos);
		mutex_unlock(&c->d->f_pos_lock);
		if (n != -EISDIR)
			c->wrong++;
		c->calls++;
		cond_resched();
	}
	kthread_unuse_mm(c->mm);
	complete(&c->done);
	return 0;
}

static loff_t g310_lseek(struct file *d, loff_t off, int whence)
{
	loff_t ret;

	mutex_lock(&d->f_pos_lock);
	ret = vfs_llseek(d, off, whence);
	mutex_unlock(&d->f_pos_lock);
	return ret;
}

/* t_readdir_2's child */
static int g310_lseek_loop(void *arg)
{
	static const loff_t array[11] = { 0, 1, 2, 3, 1023, 1024, 1025, 4095,
					  4096, 4097, 0x7fffffff };
	struct g310_child *c = arg;
	loff_t pos;
	int i;

	while (!atomic_read(&c->stop)) {
		for (i = 0; i < 11; i++)
			if (g310_lseek(c->d, array[i++], SEEK_SET) < 0)
				c->lseek_err++;
		pos = g310_lseek(c->d, 0, SEEK_CUR);
		if (pos < 0 || g310_lseek(c->d, pos, SEEK_SET) < 0)
			c->lseek_err++;
		c->calls++;
		cond_resched();
	}
	complete(&c->done);
	return 0;
}

/*
 * The parent: lseek(fd, 0, SEEK_SET); while (readdir(dir)); -- returns the
 * number of entries the walk saw, or a negative errno if a getdents failed
 * (readdir(3) returns NULL then, and the parent starts over).
 */
static int g310_walk(struct file *d, struct xfs_dirent *ents, size_t bufsize)
{
	int n, seen = 0;

	if (g310_lseek(d, 0, SEEK_SET) < 0)
		return -EINVAL;
	do {
		mutex_lock(&d->f_pos_lock);
		n = xfs_getdents(d, ents, G310_BATCH, bufsize);
		mutex_unlock(&d->f_pos_lock);
		if (n < 0)
			return n;
		seen += n;
	} while (n > 0);
	return seen;
}

static void g310_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 1; i <= G310_ENTRIES; i++) {
		snprintf(path, sizeof(path), G310_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G310_DIR);
	xfs_rmdir_settled(G310_ROOT);
}

static void g310_populate(struct kunit *test)
{
	char path[64];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G310_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g310_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G310_DIR), 0);
	for (i = 1; i <= G310_ENTRIES; i++) {
		snprintf(path, sizeof(path), G310_DIR "/%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	}
}

static void g310_run(struct kunit *test, bool lseek_child)
{
	struct g310_child c = {};
	struct xfs_dirent *ents;
	struct task_struct *t;
	unsigned long end;
	size_t bufsize;
	int walks = 0, short_walks = 0, failed_walks = 0, seen;

	g310_populate(test);
	ents = kunit_kcalloc(test, G310_BATCH, sizeof(*ents), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);

	c.d = filp_open(G310_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(c.d), "opendir: %ld",
			       PTR_ERR(c.d));
	bufsize = xfs_libc_dirbuf(c.d);
	KUNIT_ASSERT_GT(test, bufsize, 0UL);
	init_completion(&c.done);
	atomic_set(&c.stop, 0);
	if (!lseek_child) {
		/* kunit_vm_mmap() is what gives this thread an mm */
		c.buf = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
				      PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, 0);
		KUNIT_ASSERT_NE(test, c.buf, 0UL);
		c.mm = current->mm;
	}
	t = kthread_run(lseek_child ? g310_lseek_loop : g310_read_loop, &c,
			"g310-child");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	end = jiffies + msecs_to_jiffies(G310_RUN_MS);
	while (time_before(jiffies, end)) {
		seen = g310_walk(c.d, ents, bufsize);
		walks++;
		if (seen < 0)
			failed_walks++;
		else if (seen != G310_ENTRIES + 2)
			short_walks++;
		cond_resched();
	}
	atomic_set(&c.stop, 1);
	wait_for_completion(&c.done);
	filp_close(c.d, NULL);

	kunit_info(test, "%d walks (%d ended in an error, %d partial), %lu child calls\n",
		   walks, failed_walks, short_walks, c.calls);
	KUNIT_EXPECT_GT(test, c.calls, 0UL);
	if (!lseek_child) {
		KUNIT_EXPECT_EQ_MSG(test, c.wrong, 0,
				    "%d read(2) calls on the directory did not fail EISDIR",
				    c.wrong);
		KUNIT_EXPECT_EQ(test, failed_walks, 0);
		KUNIT_EXPECT_EQ_MSG(test, short_walks, 0,
				    "%d walks did not return all %d entries",
				    short_walks, G310_ENTRIES + 2);
	}
}

static void t_readdir_1_read_races_readdir(struct kunit *test)
{
	g310_run(test, false);
}

static void t_readdir_2_lseek_races_readdir(struct kunit *test)
{
	g310_run(test, true);
}

static int g310_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g310_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g310_cases[] = {
	KUNIT_CASE_SLOW(t_readdir_1_read_races_readdir),
	KUNIT_CASE_SLOW(t_readdir_2_lseek_races_readdir),
	{}
};

static struct kunit_suite g310_suite = {
	.name		= "xfstests/generic/310",
	.suite_init	= g310_suite_init,
	.suite_exit	= g310_suite_exit,
	.test_cases	= g310_cases,
};

kunit_test_suites(&g310_suite);

MODULE_DESCRIPTION("xfstests generic/310 over a loopback NFS mount");
MODULE_LICENSE("GPL");
