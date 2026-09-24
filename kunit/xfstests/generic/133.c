// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/133 over a loopback NFS mount: a reader and a writer on
 * the same file, in all four combinations of buffered and direct.
 *
 * Upstream runs four rounds. Each writes a 512 MiB file with O_DIRECT,
 * then starts a writer in the background and a reader in the foreground
 * over the whole file, each buffered or O_DIRECT, waits, and removes the
 * file. Its golden output is the four round names -- the assertion is
 * that nothing deadlocks, crashes or errors.
 *
 * Over NFS the interesting pair is mixed: a direct write invalidates the
 * client's pages for its range while a buffered reader is filling them,
 * and a buffered write leaves dirty pages that a direct reader has to
 * flush before it can read (nfs_file_direct_read() calls
 * filemap_write_and_wait_range() first). Getting that wrong shows up as
 * stale or torn data rather than as an error, so this port checks the
 * bytes the reader saw as well. Upstream writes the same bytes every
 * time; here the file is created with one pattern and overwritten with
 * another, and every byte read must be one of the two.
 *
 * Deviations: 64 MiB rather than 512 MiB -- the size only sets how long
 * the two sides overlap, and 256 MiB already takes half a minute here --
 * and the background writer is a kthread rather than a second process.
 * A kthread cannot use KUnit assertions -- they unwind through the test
 * thread's try_catch -- so the worker records its first error and the
 * test thread asserts on it afterwards.
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

#define G133_ROOT	XFS_MNT "/g133"
#define G133_FILE	G133_ROOT "/io_test"

#define G133_SIZE	(64 * 1024 * 1024)
#define G133_EXPORT	"size=335544320,nr_inodes=32768"
#define G133_CHUNK	(64 * 1024)		/* upstream's -b 64k */
#define G133_PATTERN_A	0x41
#define G133_PATTERN_B	0x42

struct g133_writer {
	struct file		*f;
	bool			direct;
	u8			pattern;
	int			err;
	struct completion	done;
};

static int g133_write_all(void *arg)
{
	struct g133_writer *w = arg;
	loff_t pos = 0;
	u8 *buf;

	buf = kmalloc(G133_CHUNK, GFP_KERNEL);
	if (!buf) {
		w->err = -ENOMEM;
		complete(&w->done);
		return 0;
	}
	memset(buf, w->pattern, G133_CHUNK);

	while (pos < G133_SIZE && !kthread_should_stop()) {
		ssize_t n;

		if (w->direct)
			n = xfs_direct_write(w->f, buf, G133_CHUNK, &pos);
		else
			n = kernel_write(w->f, buf, G133_CHUNK, &pos);
		if (n != G133_CHUNK) {
			w->err = n < 0 ? (int)n : -EIO;
			break;
		}
	}
	kfree(buf);
	complete(&w->done);
	return 0;
}

static void g133_remove_tree(void *unused)
{
	xfs_unlink(G133_FILE);
	xfs_rmdir_settled(G133_ROOT);
}

/* one round: a writer thread and a reader in this thread */
static void g133_round(struct kunit *test, bool write_direct,
		       bool read_direct, u8 pattern, const char *what)
{
	struct g133_writer w = {
		.direct = write_direct,
		.pattern = pattern,
	};
	struct file *wf, *rf;
	struct task_struct *t;
	loff_t pos = 0;
	u8 *buf;
	int i;

	init_completion(&w.done);
	buf = kunit_kmalloc(test, G133_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* xfs_io -f -d -c 'pwrite -b 64k 0 512m' */
	memset(buf, G133_PATTERN_A, G133_CHUNK);
	wf = filp_open(G133_FILE, O_RDWR | O_CREAT | O_DIRECT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(wf), "%s: create: %ld", what,
			       PTR_ERR(wf));
	while (pos < G133_SIZE)
		KUNIT_ASSERT_EQ(test, xfs_direct_write(wf, buf, G133_CHUNK, &pos),
				(ssize_t)G133_CHUNK);
	filp_close(wf, NULL);
	pos = 0;

	wf = filp_open(G133_FILE, O_RDWR | (write_direct ? O_DIRECT : 0), 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(wf), "%s: writer open: %ld", what,
			       PTR_ERR(wf));
	rf = filp_open(G133_FILE, O_RDONLY | (read_direct ? O_DIRECT : 0), 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rf), "%s: reader open: %ld", what,
			       PTR_ERR(rf));
	w.f = wf;

	t = kthread_run(g133_write_all, &w, "g133-writer");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "%s: kthread_run: %ld", what,
			       PTR_ERR(t));

	while (pos < G133_SIZE) {
		loff_t at = pos;
		ssize_t n;

		if (read_direct)
			n = xfs_direct_read(rf, buf, G133_CHUNK, &pos);
		else
			n = kernel_read(rf, buf, G133_CHUNK, &pos);
		KUNIT_EXPECT_GT_MSG(test, n, 0,
				    "%s: read at %lld returned %zd", what, at,
				    n);
		if (n <= 0)
			break;

		/*
		 * Whatever the writer has done by now, a block may hold the
		 * old pattern or the new one, but every byte in it must be
		 * from one of the patterns the file has ever held.
		 */
		for (i = 0; i < n; i++)
			if (buf[i] != G133_PATTERN_A &&
			    buf[i] != G133_PATTERN_B) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, which was never written",
					   what, at + i, buf[i]);
				i = -1;
				break;
			}
		if (i < 0)
			break;
	}

	wait_for_completion(&w.done);
	KUNIT_EXPECT_EQ_MSG(test, w.err, 0, "%s: the writer failed: %d", what,
			    w.err);

	filp_close(rf, NULL);
	filp_close(wf, NULL);

	/* rm $TEST_DIR/io_test */
	xfs_settle_fput();
	KUNIT_EXPECT_EQ(test, xfs_unlink(G133_FILE), 0);
	KUNIT_EXPECT_EQ(test, xfs_wait_for_free_bytes(G133_SIZE), 0);
}

static void a_reader_and_a_writer_share_the_file(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G133_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g133_remove_tree, NULL),
			0);

	g133_round(test, false, false, G133_PATTERN_B,
		   "buffered writer, buffered reader");
	g133_round(test, true, false, G133_PATTERN_B,
		   "direct writer, buffered reader");
	g133_round(test, false, true, G133_PATTERN_B,
		   "buffered writer, direct reader");
	g133_round(test, true, true, G133_PATTERN_B,
		   "direct writer, direct reader");
}

static int g133_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G133_EXPORT);
	return xfstests_nfs_get();
}

static void g133_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g133_cases[] = {
	KUNIT_CASE_SLOW(a_reader_and_a_writer_share_the_file),
	{}
};

static struct kunit_suite g133_suite = {
	.name		= "xfstests/generic/133",
	.suite_init	= g133_suite_init,
	.suite_exit	= g133_suite_exit,
	.test_cases	= g133_cases,
};

kunit_test_suites(&g133_suite);

MODULE_DESCRIPTION("xfstests generic/133 over a loopback NFS mount");
MODULE_LICENSE("GPL");
