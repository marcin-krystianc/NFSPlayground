// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/464 over a loopback NFS mount: rewrites, appends and
 * non-integrity syncs racing writeback.
 *
 * Upstream runs LOOP_CNT rounds of LOOP_TIME seconds. In each, PROC_CNT
 * (16) processes loop on each of three operations against a random file
 * out of MAXFILES (200):
 *
 *	do_write	xfs_io -ftc "pwrite -b 65536 0 <0..99 x 64k>": truncate
 *			the file and rewrite it with 0xcd
 *	do_append	echo "test string" >> file
 *	do_writeback	xfs_io -c "sync_range -w 0 0": start writeback of the
 *			whole file without waiting (WB_SYNC_NONE)
 *
 * and after each round unmounts and fscks the filesystem. Every error is
 * sent to /dev/null; the test is aimed at the race between a file's block
 * map changing (truncate, extension) and writeback using a mapping it
 * looked up earlier, and its pass criterion is a clean fsck.
 *
 * Over NFS there is no block map, but there is a direct equivalent: the
 * client's per-inode list of dirty nfs_page requests, which a truncate
 * trims, a rewrite fills, an append extends and a sync_file_range flushes.
 * There is no fsck either, so after the rounds the port checks what one
 * would: every byte of every file is 0xcd, a byte of "test string\n", or
 * zero (a hole left by one worker's truncate under another's rewrite).
 * Errors other than ENOSPC (a full export, which upstream also ignores)
 * and ENOENT (sync_range on a file nobody has created yet) are reported.
 *
 * Deviations: 2 rounds of 3 s rather than 10 of 5 s; each process is a
 * kthread; the export is 320 MiB, and a rewrite that runs out of it just
 * fails as upstream's would.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/prandom.h>
#include <linux/random.h>

#include "xfstests_nfs_fixture.h"

#define G464_ROOT	XFS_MNT "/g464"
#define G464_EXPORT	"size=335544320,nr_inodes=32768"
#define G464_MAXFILES	200
#define G464_BLOCK_SZ	65536
#define G464_LOOP_CNT	2
#define G464_LOOP_MS	3000
#define G464_PROC_CNT	16
#define G464_WORKERS	(3 * G464_PROC_CNT)

static const char g464_text[] = "test string\n";

enum { G464_WRITE, G464_APPEND, G464_WRITEBACK };

struct g464_worker {
	int			op;
	atomic_t		*stop;
	u8			*buf;
	struct rnd_state	rnd;
	unsigned long		ops;
	int			bad_err;
	struct completion	done;
};

static void g464_path(char *buf, size_t size, u32 n)
{
	snprintf(buf, size, G464_ROOT "/%u", n);
}

static void g464_note(struct g464_worker *w, int err)
{
	if (err && err != -ENOSPC && err != -ENOENT && !w->bad_err)
		w->bad_err = err;
}

/* do_write: xfs_io -ftc "pwrite -b 65536 0 $filesize" */
static void g464_do_write(struct g464_worker *w, const char *path)
{
	loff_t filesize = (prandom_u32_state(&w->rnd) % 100) *
			  (loff_t)G464_BLOCK_SZ;
	struct file *f;
	loff_t pos = 0;

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (IS_ERR(f)) {
		g464_note(w, PTR_ERR(f));
		return;
	}
	while (pos < filesize) {
		ssize_t n = kernel_write(f, w->buf, G464_BLOCK_SZ, &pos);

		if (n != G464_BLOCK_SZ) {
			g464_note(w, n < 0 ? (int)n : -EIO);
			break;
		}
	}
	filp_close(f, NULL);
}

/* do_append: echo "test string" >> file */
static void g464_do_append(struct g464_worker *w, const char *path)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	f = filp_open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (IS_ERR(f)) {
		g464_note(w, PTR_ERR(f));
		return;
	}
	n = kernel_write(f, g464_text, sizeof(g464_text) - 1, &pos);
	if (n != sizeof(g464_text) - 1)
		g464_note(w, n < 0 ? (int)n : -EIO);
	filp_close(f, NULL);
}

/* do_writeback: xfs_io -c "sync_range -w 0 0" */
static void g464_do_writeback(struct g464_worker *w, const char *path)
{
	struct file *f = filp_open(path, O_RDWR, 0);

	if (IS_ERR(f)) {
		g464_note(w, PTR_ERR(f));
		return;
	}
	g464_note(w, sync_file_range(f, 0, 0, SYNC_FILE_RANGE_WRITE));
	filp_close(f, NULL);
}

static int g464_loop(void *arg)
{
	struct g464_worker *w = arg;
	char path[64];

	while (!atomic_read(w->stop)) {
		g464_path(path, sizeof(path),
			  prandom_u32_state(&w->rnd) % G464_MAXFILES);
		switch (w->op) {
		case G464_WRITE:
			g464_do_write(w, path);
			break;
		case G464_APPEND:
			g464_do_append(w, path);
			break;
		default:
			g464_do_writeback(w, path);
			break;
		}
		w->ops++;
		cond_resched();
	}
	complete(&w->done);
	return 0;
}

static void g464_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_settle_fput();
	for (i = 0; i < G464_MAXFILES; i++) {
		g464_path(path, sizeof(path), i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G464_ROOT);
}

static bool g464_byte_ok(u8 c)
{
	return c == 0xcd || c == 0 || memchr(g464_text, c, sizeof(g464_text) - 1);
}

static void writes_appends_and_syncs_race_writeback(struct kunit *test)
{
	struct g464_worker *w;
	struct task_struct *t;
	atomic_t stop;
	char path[64];
	u8 *rd;
	int loop, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G464_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g464_remove_tree, NULL),
			0);
	w = kunit_kcalloc(test, G464_WORKERS, sizeof(*w), GFP_KERNEL);
	rd = kunit_kmalloc(test, G464_BLOCK_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);

	for (loop = 0; loop < G464_LOOP_CNT; loop++) {
		atomic_set(&stop, 0);
		for (i = 0; i < G464_WORKERS; i++) {
			w[i].op = i % 3;
			w[i].stop = &stop;
			if (!w[i].buf) {
				w[i].buf = kunit_kmalloc(test, G464_BLOCK_SZ,
							 GFP_KERNEL);
				KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w[i].buf);
				memset(w[i].buf, 0xcd, G464_BLOCK_SZ);
			}
			prandom_seed_state(&w[i].rnd, get_random_u64());
			init_completion(&w[i].done);
			t = kthread_run(g464_loop, &w[i], "g464-%d", i);
			KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t),
					       "kthread_run: %ld", PTR_ERR(t));
		}
		msleep(G464_LOOP_MS);
		atomic_set(&stop, 1);
		for (i = 0; i < G464_WORKERS; i++)
			wait_for_completion(&w[i].done);
	}

	for (i = 0; i < G464_WORKERS; i++) {
		KUNIT_EXPECT_EQ_MSG(test, w[i].bad_err, 0,
				    "worker %d (op %d) hit %d", i, w[i].op,
				    w[i].bad_err);
		KUNIT_EXPECT_GT_MSG(test, w[i].ops, 0UL,
				    "worker %d (op %d) never ran", i, w[i].op);
	}

	/* not upstream, in place of its fsck: no byte nobody wrote */
	for (i = 0; i < G464_MAXFILES; i++) {
		struct kstat st;
		loff_t off;
		ssize_t n;
		int j;

		g464_path(path, sizeof(path), i);
		if (xfs_kstat(path, &st))
			continue;
		for (off = 0; off < st.size; off += n) {
			n = xfs_read_range(path, rd, G464_BLOCK_SZ, off);
			KUNIT_ASSERT_GT_MSG(test, n, 0L, "file %d: read at %lld",
					    i, off);
			for (j = 0; j < n; j++)
				if (!g464_byte_ok(rd[j])) {
					KUNIT_FAIL(test,
						   "file %d: byte %lld is %02x, which nobody wrote",
						   i, off + j, rd[j]);
					return;
				}
		}
	}
}

static int g464_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G464_EXPORT);
	return xfstests_nfs_get();
}

static void g464_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g464_cases[] = {
	KUNIT_CASE_SLOW(writes_appends_and_syncs_race_writeback),
	{}
};

static struct kunit_suite g464_suite = {
	.name		= "xfstests/generic/464",
	.suite_init	= g464_suite_init,
	.suite_exit	= g464_suite_exit,
	.test_cases	= g464_cases,
};

kunit_test_suites(&g464_suite);

MODULE_DESCRIPTION("xfstests generic/464 over a loopback NFS mount");
MODULE_LICENSE("GPL");
