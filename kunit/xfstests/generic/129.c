// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/129 over a loopback NFS mount: looptest read/write loops.
 *
 * Upstream runs src/looptest four times. Its flags are not what they look
 * like (looptest.c:97 and its usage text): -s is SEQUENTIAL, not sync; -f is
 * flush and is never passed here; -o is open/close per iteration; -t is
 * TRUNCATE, which is ftruncate(f, 0) every iteration (looptest.c:42).
 *
 *   1. -i 100000 -r -w -b 8192  -s
 *   2. -i 10000  -r -w -b 102400 -s -t
 *   3. -i 50000  -r -w -b 256   -s
 *   4. -i 2000   -r -w -b 8192  -s -o
 *
 * So every set is sequential: iteration i writes one buffer at i*bufsize,
 * seeks back and reads it. Set 2 additionally truncates to zero after each
 * read while the write offset keeps climbing, which over NFS is a SETATTR
 * (size 0) interleaved with WRITEs to ever-higher offsets on the same open
 * file. Set 4 reopens the file every iteration, so each read has to come
 * from a fresh struct file (and, for NFS, a revalidated inode) rather than
 * from the one that did the write.
 *
 * The loop below is that loop, including the order in which looptest seeks:
 * on a sequential run the write goes at the current offset and only the read
 * seeks back. Upstream's only pass criterion is that no I/O errors and no
 * short or past-EOF reads occur; the port additionally compares the bytes
 * read against the buffer looptest wrote (buf[i] = i & 127, looptest.c:163),
 * which is the same data, checked rather than discarded.
 *
 * Deviation: iteration counts are scaled down (2000/500/2000/500 against
 * upstream's 100000/10000/50000/2000); every flag combination is kept.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G129_ROOT	XFS_MNT "/g129"
#define G129_FILE	G129_ROOT "/loop"

static void g129_remove_tree(void *unused)
{
	xfs_unlink(G129_FILE);
	xfs_rmdir(G129_ROOT);
}

/*
 * One looptest run. @seq/@trunc/@openclose are its -s/-t/-o; read and write
 * are always on (-r -w), as in all four upstream invocations.
 */
static void g129_loop(struct kunit *test, int iters, u32 bs, bool seq,
		      bool trunc, bool openclose, int setno)
{
	struct file *f = NULL;
	u8 *wr, *rd;
	loff_t seek_to = 0, pos = 0;
	int i;
	u32 j;

	wr = kunit_kmalloc(test, bs, GFP_KERNEL);
	rd = kunit_kmalloc(test, bs, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, wr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);

	/* looptest.c:163 -- one buffer, filled once, reused every iteration */
	for (j = 0; j < bs; j++)
		wr[j] = (u8)(j & 127);

	xfs_unlink(G129_FILE);

	for (i = 0; i < iters; i++) {
		ssize_t n;

		if (openclose || !i) {
			f = filp_open(G129_FILE, O_RDWR | O_CREAT, 0644);
			KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
					       "set %d iter %d: open: %ld",
					       setno, i, PTR_ERR(f));
			pos = 0;
		}

		if (openclose && seq)
			pos = seek_to;

		/* write: a non-sequential run seeks first, a sequential one
		 * writes wherever the offset already is
		 */
		if (!seq)
			pos = seek_to;
		n = kernel_write(f, wr, bs, &pos);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)bs,
				    "set %d iter %d: write at %lld returned %zd",
				    setno, i, seek_to, n);

		/* read: seeks back to the block just written */
		pos = seek_to;
		n = kernel_read(f, rd, bs, &pos);
		KUNIT_ASSERT_NE_MSG(test, n, (ssize_t)0,
				    "set %d iter %d: read past EOF at %lld",
				    setno, i, seek_to);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)bs,
				    "set %d iter %d: short read at %lld: %zd",
				    setno, i, seek_to, n);
		KUNIT_ASSERT_EQ_MSG(test, memcmp(wr, rd, bs), 0,
				    "set %d iter %d: the block at %lld read back changed",
				    setno, i, seek_to);

		if (trunc) {
			KUNIT_ASSERT_EQ_MSG(test, xfs_ftruncate(f, 0), 0,
					    "set %d iter %d: ftruncate", setno, i);
			pos = 0;
		}

		if (seq) {
			seek_to += bs;
			if (trunc)
				pos = seek_to;
		}

		if (openclose) {
			filp_close(f, NULL);
			f = NULL;
		}
	}
	if (f)
		filp_close(f, NULL);
}

static void looptest_parameter_sets_read_back_what_they_wrote(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G129_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g129_remove_tree, NULL),
			0);

	/* -i .. -r -w -b 8192 -s */
	g129_loop(test, 2000, 8192, true, false, false, 1);
	/* -i .. -t -r -w -s -b 102400 */
	g129_loop(test, 500, 102400, true, true, false, 2);
	/* -i .. -r -w -b 256 -s */
	g129_loop(test, 2000, 256, true, false, false, 3);
	/* -i .. -o -r -w -b 8192 -s */
	g129_loop(test, 500, 8192, true, false, true, 4);
}

static int g129_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g129_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g129_cases[] = {
	KUNIT_CASE_SLOW(looptest_parameter_sets_read_back_what_they_wrote),
	{}
};

static struct kunit_suite g129_suite = {
	.name		= "xfstests/generic/129",
	.suite_init	= g129_suite_init,
	.suite_exit	= g129_suite_exit,
	.test_cases	= g129_cases,
};

kunit_test_suites(&g129_suite);

MODULE_DESCRIPTION("xfstests generic/129 over a loopback NFS mount");
MODULE_LICENSE("GPL");
