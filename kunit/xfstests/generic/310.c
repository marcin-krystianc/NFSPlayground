// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/310 over a loopback NFS mount: read(2) and readdir on
 * the same directory file at the same time.
 *
 * src/t_readdir_1 opens a directory, forks, and then has one side call
 * read(2) on the directory's file descriptor in a loop while the other
 * seeks it back to zero and walks it with readdir. (t_readdir_2 swaps
 * which side seeks.) read(2) on a directory fails -- EISDIR on Linux --
 * but it takes the same f_pos and the same inode lock on the way there,
 * and on ext3 the pair used to produce "bad entry in directory" errors.
 * The test's pass criterion is that dmesg gains no BUG, NULL dereference,
 * WARNING or lockdep report.
 *
 * Over NFS the same fd carries the client's readdir cookie and its page
 * cache of directory entries, so a read(2) that touched f_pos, or a seek
 * that raced the walk, would show up as entries returned twice or not at
 * all. The port therefore checks what upstream cannot: every walk must
 * return exactly the entries that exist, and every read(2) must fail with
 * EISDIR rather than returning bytes.
 *
 * Deviations: bounded rounds instead of upstream's run-until-killed, and
 * a kthread instead of a forked child -- they share the struct file
 * either way, which is what the test is about.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>

#include "xfstests_nfs_fixture.h"

#define G310_ROOT	XFS_MNT "/g310"
#define G310_DIR	G310_ROOT "/tmp"
#define G310_ENTRIES	32
#define G310_ROUNDS	40

struct g310_reader {
	struct file		*d;
	struct mm_struct	*mm;
	unsigned long		buf;		/* a user address to read into */
	atomic_t		stop;
	int			wrong;		/* reads that did not fail */
	int			err;		/* an unexpected errno */
	unsigned long		reads;
	struct completion	done;
};

/*
 * read(2), not kernel_read(): a directory's file operations wire up both
 * ->read (generic_read_dir, which is what returns EISDIR) and
 * ->iterate_shared, and __kernel_read() refuses any file with both ->read
 * and ->read_iter rather than calling it. So this borrows the test
 * thread's mm and reads into a user address, which is the syscall's own
 * path.
 */
static int g310_read_loop(void *arg)
{
	struct g310_reader *r = arg;

	kthread_use_mm(r->mm);
	while (!atomic_read(&r->stop)) {
		loff_t pos = 0;
		ssize_t n = vfs_read(r->d, (char __user *)r->buf, 100, &pos);

		r->reads++;
		if (n >= 0)
			r->wrong++;
		else if (n != -EISDIR)
			r->err = (int)n;
		cond_resched();
	}
	kthread_unuse_mm(r->mm);
	complete(&r->done);
	return 0;
}

struct g310_walk {
	struct dir_context	ctx;
	int			entries;
	int			alien;
	int			total;		/* including . and .. */
};

static bool g310_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g310_walk *w = container_of(ctx, struct g310_walk, ctx);

	w->total++;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (len < 2 || name[0] != 'e')
		w->alien++;
	else
		w->entries++;
	return true;
}

static void g310_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < G310_ENTRIES; i++) {
		snprintf(path, sizeof(path), G310_DIR "/e%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G310_DIR);
	xfs_rmdir_settled(G310_ROOT);
}

static void reads_racing_readdir_on_one_fd(struct kunit *test)
{
	struct g310_reader r = {};
	struct task_struct *t;
	char path[64];
	struct file *d;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G310_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g310_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G310_DIR), 0);

	for (i = 0; i < G310_ENTRIES; i++) {
		snprintf(path, sizeof(path), G310_DIR "/e%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	}

	d = filp_open(G310_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));

	init_completion(&r.done);
	atomic_set(&r.stop, 0);
	r.d = d;
	/* kunit_vm_mmap() is what gives this thread an mm, so take the
	 * pointer only after it has run.
	 */
	r.buf = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE_MSG(test, r.buf, 0UL, "anonymous mapping failed");
	r.mm = current->mm;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r.mm);
	t = kthread_run(g310_read_loop, &r, "g310-reader");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* the point is that the two run together, so wait for the reader to
	 * get going rather than racing it to the finish
	 */
	for (i = 0; i < 100 && !r.reads; i++)
		msleep(10);

	for (i = 0; i < G310_ROUNDS; i++) {
		struct g310_walk w = { .ctx.actor = g310_actor };
		int before;

		KUNIT_ASSERT_EQ_MSG(test, vfs_llseek(d, 0, SEEK_SET), 0LL,
				    "round %d: seek to 0 failed", i);
		/*
		 * One iterate_dir() is one getdents(2): it returns a batch,
		 * not the whole directory, so keep going until a pass adds
		 * nothing.
		 */
		do {
			before = w.total;
			KUNIT_ASSERT_EQ_MSG(test, iterate_dir(d, &w.ctx), 0,
					    "round %d: iterate_dir failed", i);
		} while (w.total > before);
		KUNIT_EXPECT_EQ_MSG(test, w.entries, G310_ENTRIES,
				    "round %d returned %d entries, expected %d",
				    i, w.entries, G310_ENTRIES);
		KUNIT_EXPECT_EQ_MSG(test, w.alien, 0,
				    "round %d returned %d unexpected names", i,
				    w.alien);
	}

	atomic_set(&r.stop, 1);
	wait_for_completion(&r.done);
	filp_close(d, NULL);

	KUNIT_EXPECT_GT_MSG(test, r.reads, 0UL, "the reader never ran");
	KUNIT_EXPECT_EQ_MSG(test, r.wrong, 0,
			    "%d read(2) calls on the directory succeeded",
			    r.wrong);
	KUNIT_EXPECT_EQ_MSG(test, r.err, 0,
			    "read(2) on the directory returned %d, expected EISDIR",
			    r.err);
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
	KUNIT_CASE_SLOW(reads_racing_readdir_on_one_fd),
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
