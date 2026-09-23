// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/364 over a loopback NFS mount: direct writes and
 * fsync on the same open file at the same time.
 *
 * src/dio-write-fsync-same-fd runs two threads sharing one file
 * descriptor: one does O_DIRECT writes in a loop, the other fsyncs in a
 * loop. Upstream runs it under a ten-second timeout, because the failure
 * mode it was written for (btrfs commit cd9253c23aed) is a deadlock, not
 * a wrong answer.
 *
 * Over NFS an fsync is nfs_file_fsync() -> a COMMIT, and a direct write
 * is nfs_direct_write() with its own requests; both touch the same
 * nfs_inode's commit machinery, and a direct write that is treated as
 * needing a commit while a commit is already in flight is the
 * interesting overlap. As upstream, a hang is the failure -- KUnit's
 * per-case timeout is what reports it -- but the port also checks the
 * file's contents afterwards, which the original does not.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G364_ROOT	XFS_MNT "/g364"
#define G364_FILE	G364_ROOT "/dio-write-fsync-same-fd"
#define G364_SERVER	XFS_EXPORT "/g364/dio-write-fsync-same-fd"

#define G364_CHUNK	4096
#define G364_ROUNDS	256
#define G364_BYTE	0x64

struct g364_writer {
	struct file		*f;
	int			err;
	struct completion	done;
};

static int g364_write_loop(void *arg)
{
	struct g364_writer *w = arg;
	u8 *buf;
	int i;

	buf = kmalloc(G364_CHUNK, GFP_KERNEL);
	if (!buf) {
		w->err = -ENOMEM;
		complete(&w->done);
		return 0;
	}
	memset(buf, G364_BYTE, G364_CHUNK);

	for (i = 0; i < G364_ROUNDS; i++) {
		loff_t pos = (loff_t)i * G364_CHUNK;
		ssize_t n = xfs_direct_write(w->f, buf, G364_CHUNK, &pos);

		if (n != G364_CHUNK) {
			w->err = n < 0 ? (int)n : -EIO;
			break;
		}
	}
	kfree(buf);
	complete(&w->done);
	return 0;
}

static void g364_remove_tree(void *unused)
{
	xfs_unlink(G364_FILE);
	xfs_rmdir_settled(G364_ROOT);
}

static void direct_writes_and_fsyncs_on_one_fd(struct kunit *test)
{
	struct g364_writer w = {};
	struct task_struct *t;
	struct file *f;
	u8 *buf;
	int i, syncs = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G364_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g364_remove_tree, NULL),
			0);

	f = filp_open(G364_FILE, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	init_completion(&w.done);
	w.f = f;
	t = kthread_run(g364_write_loop, &w, "g364-writer");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* the other thread: fsync the same file until the writer is done */
	while (!completion_done(&w.done)) {
		KUNIT_ASSERT_EQ_MSG(test, vfs_fsync(f, 0), 0,
				    "fsync failed after %d syncs", syncs);
		syncs++;
		cond_resched();
	}
	wait_for_completion(&w.done);
	KUNIT_EXPECT_EQ_MSG(test, w.err, 0, "the writer failed: %d", w.err);
	KUNIT_EXPECT_GT_MSG(test, syncs, 0, "no fsync ever ran");

	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	/* every byte the writer wrote is on the server */
	buf = kunit_kmalloc(test, G364_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G364_ROUNDS; i++) {
		int j;

		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G364_SERVER, buf, G364_CHUNK,
					       (loff_t)i * G364_CHUNK),
				(ssize_t)G364_CHUNK);
		for (j = 0; j < G364_CHUNK; j++)
			if (buf[j] != G364_BYTE) {
				KUNIT_FAIL(test,
					   "server byte %lld is %02x, expected %02x",
					   (loff_t)i * G364_CHUNK + j, buf[j],
					   G364_BYTE);
				return;
			}
	}
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
