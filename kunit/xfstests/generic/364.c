// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/364 over a loopback NFS mount: direct writes and
 * fsync on the same open file at the same time.
 *
 * src/dio-write-fsync-same-fd opens a file O_WRONLY | O_CREAT | O_TRUNC |
 * O_DIRECT, starts a thread that fsync()s it in a loop, and pwrite()s one
 * page at offset 0 over and over on the same descriptor. Upstream runs it
 * under a ten-second timeout, because the failure it was written for
 * (btrfs commit cd9253c23aed) is a deadlock, not a wrong answer: the
 * program only exits on an error.
 *
 * Over NFS an fsync is nfs_file_fsync() -> a COMMIT, and a direct write
 * is nfs_direct_write() with its own requests; both touch the same
 * nfs_inode's commit machinery, and a direct write that is treated as
 * needing a commit while a commit is already in flight is the
 * interesting overlap. As upstream, a hang is the failure -- KUnit's
 * per-case timeout is what reports it -- but the port also checks the
 * page on the server afterwards, which the original does not.
 *
 * Deviation: the loops run for 5 s rather than until a 10 s timeout.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/jiffies.h>

#include "xfstests_nfs_fixture.h"

#define G364_ROOT	XFS_MNT "/g364"
#define G364_FILE	G364_ROOT "/dio-write-fsync-same-fd"
#define G364_SERVER	XFS_EXPORT "/g364/dio-write-fsync-same-fd"

#define G364_CHUNK	4096
#define G364_RUN_MS	5000
#define G364_BYTE	0x64

struct g364_fsyncer {
	struct file		*f;
	atomic_t		stop;
	unsigned long		syncs;
	int			err;
	struct completion	done;
};

/* fsync_loop() */
static int g364_fsync_loop(void *arg)
{
	struct g364_fsyncer *s = arg;

	while (!atomic_read(&s->stop)) {
		int err = vfs_fsync(s->f, 0);

		if (err) {
			s->err = err;
			break;
		}
		s->syncs++;
		cond_resched();
	}
	complete(&s->done);
	return 0;
}

static void g364_remove_tree(void *unused)
{
	xfs_unlink(G364_FILE);
	xfs_rmdir_settled(G364_ROOT);
}

static void direct_writes_and_fsyncs_on_one_fd(struct kunit *test)
{
	struct g364_fsyncer s = {};
	struct task_struct *t;
	unsigned long end, writes = 0;
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G364_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g364_remove_tree, NULL),
			0);
	buf = kunit_kmalloc(test, G364_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, G364_BYTE, G364_CHUNK);

	f = filp_open(G364_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	init_completion(&s.done);
	atomic_set(&s.stop, 0);
	s.f = f;
	t = kthread_run(g364_fsync_loop, &s, "g364-fsync");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* while (1) do_write(fd, write_buf, pagesize, 0); */
	end = jiffies + msecs_to_jiffies(G364_RUN_MS);
	while (time_before(jiffies, end)) {
		loff_t pos = 0;
		ssize_t n = xfs_direct_write(f, buf, G364_CHUNK, &pos);

		if (n != G364_CHUNK) {
			KUNIT_FAIL(test, "Write failed: %zd", n);
			break;
		}
		writes++;
		cond_resched();
	}
	atomic_set(&s.stop, 1);
	wait_for_completion(&s.done);
	filp_close(f, NULL);

	KUNIT_EXPECT_EQ_MSG(test, s.err, 0, "Fsync failed: %d", s.err);
	KUNIT_EXPECT_GT(test, writes, 0UL);
	KUNIT_EXPECT_GT_MSG(test, s.syncs, 0UL, "no fsync ever ran");

	/* not upstream: the page is on the server */
	memset(buf, 0, G364_CHUNK);
	KUNIT_ASSERT_EQ(test, xfs_read_range(G364_SERVER, buf, G364_CHUNK, 0),
			(ssize_t)G364_CHUNK);
	KUNIT_EXPECT_TRUE_MSG(test, !memchr_inv(buf, G364_BYTE, G364_CHUNK),
			      "the server does not hold the written page");
}

static int g364_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g364_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g364_cases[] = {
	KUNIT_CASE_SLOW(direct_writes_and_fsyncs_on_one_fd),
	{}
};

static struct kunit_suite g364_suite = {
	.name		= "xfstests/generic/364",
	.suite_init	= g364_suite_init,
	.suite_exit	= g364_suite_exit,
	.test_cases	= g364_cases,
};

kunit_test_suites(&g364_suite);

MODULE_DESCRIPTION("xfstests generic/364 over a loopback NFS mount");
MODULE_LICENSE("GPL");
