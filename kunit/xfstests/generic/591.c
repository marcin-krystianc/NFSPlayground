// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/591 over a loopback NFS mount: splice from a file,
 * with and without O_DIRECT, into a pipe that is read sequentially or
 * concurrently.
 *
 * src/splice-test writes 150 sectors of 'x' to a file opened O_RDWR (and
 * O_DIRECT unless -d), seeks back to 0, and splices the whole file into a
 * pipe. Without -r it alternates: splice what fits, read that much back.
 * With -r a forked child reads the pipe while the parent splices into it.
 * Upstream runs the four combinations; the O_DIRECT ones use
 * min_dio_alignment as the sector size, which on NFS is the page size
 * (the program's O_TMPFILE probe fails and TEST_DEV is not a block
 * device), the others the program's default of 512.
 *
 * Over NFS a splice from an O_DIRECT file goes through copy_splice_read()
 * into nfs_file_direct_read() with a bvec iterator; without O_DIRECT it is
 * nfs_file_splice_read(). The generic/249 port covers only the latter.
 *
 * Deviations: the pipe comes from create_pipe_files() and the splice is
 * do_splice(), the body of splice(2). The forked reader is a kthread. The
 * port also checks that every byte read from the pipe is 'x'.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/splice.h>
#include <linux/pipe_fs_i.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G591_ROOT	XFS_MNT "/g591"
#define G591_FILE	G591_ROOT "/a"

struct g591_reader {
	struct file		*pipe;
	size_t			size;
	int			err;
	size_t			bad;	/* bytes that were not 'x' */
	struct completion	done;
};

/* splice-test's read_from_pipe(), plus a content check */
static void g591_read_from_pipe(struct g591_reader *r)
{
	size_t left = r->size;
	loff_t pos = 0;
	u8 *buf;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		r->err = -ENOMEM;
		return;
	}
	while (left) {
		ssize_t n = kernel_read(r->pipe, buf,
					min_t(size_t, left, PAGE_SIZE), &pos);
		ssize_t i;

		if (n <= 0) {
			r->err = n ? (int)n : -ENODATA;	/* unexpected EOF */
			break;
		}
		for (i = 0; i < n; i++)
			if (buf[i] != 'x')
				r->bad++;
		left -= n;
	}
	kfree(buf);
}

static int g591_reader_thread(void *arg)
{
	struct g591_reader *r = arg;

	g591_read_from_pipe(r);
	complete(&r->done);
	return 0;
}

static void g591_remove_tree(void *unused)
{
	xfs_unlink(G591_FILE);
	xfs_rmdir_settled(G591_ROOT);
}

static void g591_run(struct kunit *test, bool concurrent, bool direct,
		     unsigned int sector)
{
	size_t size = 150 * sector;
	struct g591_reader r = { .size = size };
	struct file *pipe[2], *f;
	const char *what;
	loff_t pos = 0;
	size_t left;
	ssize_t n;
	u8 *buf;
	int err;

	what = concurrent ? (direct ? "concurrent reader with O_DIRECT" :
			     "concurrent reader without O_DIRECT") :
			    (direct ? "sequential reader with O_DIRECT" :
			     "sequential reader without O_DIRECT");

	buf = kunit_kmalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'x', size);

	f = filp_open(G591_FILE, O_CREAT | O_TRUNC | O_RDWR |
		      (direct ? O_DIRECT : 0), 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
			       PTR_ERR(f));
	n = direct ? xfs_direct_write(f, buf, size, &pos) :
		     kernel_write(f, buf, size, &pos);
	if (n != size) {
		filp_close(f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "%s: write: %zd", what, n);
	}
	/* lseek(fd, 0, SEEK_SET) */
	f->f_pos = 0;

	err = create_pipe_files(pipe, 0);
	if (err) {
		filp_close(f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "%s: pipe: %d", what, err);
	}
	r.pipe = pipe[0];
	init_completion(&r.done);

	if (concurrent) {
		struct task_struct *t;

		t = kthread_run(g591_reader_thread, &r, "g591-reader");
		if (IS_ERR(t)) {
			r.err = PTR_ERR(t);
			complete(&r.done);
		}
	}

	for (left = size; left; left -= n) {
		n = do_splice(f, NULL, pipe[1], NULL, left, SPLICE_F_MOVE);
		if (n <= 0) {
			KUNIT_FAIL(test, "%s: splice: %zd", what, n);
			break;
		}
		if (!concurrent) {
			r.size = n;
			g591_read_from_pipe(&r);
			if (r.err)
				break;
		}
	}

	fput(pipe[1]);
	if (concurrent)
		wait_for_completion(&r.done);
	fput(pipe[0]);
	filp_close(f, NULL);

	KUNIT_EXPECT_EQ_MSG(test, r.err, 0, "%s: read from pipe: %d", what,
			    r.err);
	KUNIT_EXPECT_EQ_MSG(test, r.bad, (size_t)0,
			    "%s: %zu bytes read from the pipe were not 'x'",
			    what, r.bad);

	/* unlink(filename) */
	KUNIT_EXPECT_EQ(test, xfs_unlink(G591_FILE), 0);
	xfs_settle_fput();
}

static void splice_into_a_pipe(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G591_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g591_remove_tree, NULL),
			0);

	/* splice-test -s $diosize -r */
	g591_run(test, true, true, PAGE_SIZE);
	/* splice-test -rd */
	g591_run(test, true, false, 512);
	/* splice-test -s $diosize */
	g591_run(test, false, true, PAGE_SIZE);
	/* splice-test -d */
	g591_run(test, false, false, 512);
}

static int g591_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g591_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g591_cases[] = {
	KUNIT_CASE(splice_into_a_pipe),
	{}
};

static struct kunit_suite g591_suite = {
	.name		= "xfstests/generic/591",
	.suite_init	= g591_suite_init,
	.suite_exit	= g591_suite_exit,
	.test_cases	= g591_cases,
};

kunit_test_suites(&g591_suite);

MODULE_DESCRIPTION("xfstests generic/591 over a loopback NFS mount");
MODULE_LICENSE("GPL");
