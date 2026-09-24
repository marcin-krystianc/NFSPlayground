// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/615 over a loopback NFS mount: st_blocks never reads
 * as zero while a file is being rewritten.
 *
 * Upstream creates a 64k file with xfs_io -f -s, then keeps overwriting
 * it -- 2000 xfs_io -s runs, then 2000 xfs_io -d runs, each opening the
 * file and writing 64k in one pwrite -- while another process stats it in
 * a loop, and fails if stat ever reports zero allocated blocks. A file
 * that has data must never appear to have none, however briefly, or du
 * and friends see it vanish mid-writeback.
 *
 * Over NFS st_blocks is the server's space_used attribute as the client
 * last saw it, so the question becomes: while the client is writing, can
 * a GETATTR (or a cached attribute update from a WRITE reply) leave the
 * inode with i_blocks zeroed? nfs_update_inode() only takes the fields
 * the server actually returned, and a zero here would mean it took a
 * field the reply did not carry.
 *
 * The stat loop is a kthread that counts violations rather than
 * asserting -- KUnit assertions belong to the test thread. Upstream's loop
 * stops at the first one and the writers stop with it; here the writers
 * run on and the count is checked at the end.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>

#include "xfstests_nfs_fixture.h"

#define G615_ROOT	XFS_MNT "/g615"
#define G615_FILE	G615_ROOT "/foo"
#define G615_SIZE	(64 * 1024)
#define G615_ROUNDS	2000

struct g615_watcher {
	atomic_t		stop;
	unsigned long		stats;
	int			zero_blocks;
	int			err;
	struct completion	done;
};

static int g615_stat_loop(void *arg)
{
	struct g615_watcher *w = arg;

	while (!atomic_read(&w->stop)) {
		struct kstat st;
		int err = xfs_kstat(G615_FILE, &st);

		if (err) {
			w->err = err;
			break;
		}
		w->stats++;
		if (st.blocks == 0)
			w->zero_blocks++;
		cond_resched();
	}
	complete(&w->done);
	return 0;
}

static void g615_remove_tree(void *unused)
{
	xfs_settle_fput();
	xfs_unlink(G615_FILE);
	xfs_rmdir_settled(G615_ROOT);
}

static void g615_rewrite(struct kunit *test, bool direct, u8 *buf,
			 const char *what)
{
	int i;

	/* xfs_io {-s|-d} -c "pwrite -b 64K 0 64K" foo, 2000 times */
	for (i = 0; i < G615_ROUNDS; i++) {
		struct file *f;
		loff_t pos = 0;
		ssize_t n;

		f = filp_open(G615_FILE,
			      O_RDWR | (direct ? O_DIRECT : O_SYNC), 0);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
				       PTR_ERR(f));
		if (direct)
			n = xfs_direct_write(f, buf, G615_SIZE, &pos);
		else
			n = kernel_write(f, buf, G615_SIZE, &pos);
		filp_close(f, NULL);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G615_SIZE,
				    "%s: round %d wrote %zd", what, i, n);
	}
}

static void st_blocks_is_never_zero_while_rewriting(struct kunit *test)
{
	struct g615_watcher w = {};
	struct task_struct *t;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G615_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g615_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G615_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, G615_SIZE);
	/* xfs_io -f -s -c "pwrite -b 64K 0 64K" foo */
	{
		struct file *f;
		loff_t pos = 0;

		f = filp_open(G615_FILE, O_RDWR | O_CREAT | O_SYNC, 0600);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create: %ld",
				       PTR_ERR(f));
		KUNIT_EXPECT_EQ(test, kernel_write(f, buf, G615_SIZE, &pos),
				(ssize_t)G615_SIZE);
		filp_close(f, NULL);
	}

	init_completion(&w.done);
	atomic_set(&w.stop, 0);
	t = kthread_run(g615_stat_loop, &w, "g615-stat");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	g615_rewrite(test, false, buf, "buffered writes");
	g615_rewrite(test, true, buf, "direct writes");

	atomic_set(&w.stop, 1);
	wait_for_completion(&w.done);

	KUNIT_EXPECT_EQ_MSG(test, w.err, 0, "the stat loop failed: %d", w.err);
	KUNIT_EXPECT_GT_MSG(test, w.stats, 0UL, "the stat loop never ran");
	KUNIT_EXPECT_EQ_MSG(test, w.zero_blocks, 0,
			    "stat reported zero blocks %d times out of %lu",
			    w.zero_blocks, w.stats);
}

static int g615_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g615_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g615_cases[] = {
	KUNIT_CASE_SLOW(st_blocks_is_never_zero_while_rewriting),
	{}
};

static struct kunit_suite g615_suite = {
	.name		= "xfstests/generic/615",
	.suite_init	= g615_suite_init,
	.suite_exit	= g615_suite_exit,
	.test_cases	= g615_cases,
};

kunit_test_suites(&g615_suite);

MODULE_DESCRIPTION("xfstests generic/615 over a loopback NFS mount");
MODULE_LICENSE("GPL");
