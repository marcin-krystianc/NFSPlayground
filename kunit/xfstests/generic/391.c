// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/391 over a loopback NFS mount: two direct readers in
 * the same extents.
 *
 * src/dio-interleaved opens a fallocated file O_DIRECT and starts two
 * threads that walk it backwards in lockstep, each reading one half of
 * every extent, so the two reads always fall inside the same extent at
 * the same moment. Btrfs' direct-I/O get_block returned spurious -EEXIST
 * for the second of the pair. Upstream drops the caches first so the
 * extent map has to be rebuilt.
 *
 * Over NFS there is no extent map, but the pair is still a real case:
 * two concurrent direct reads of adjacent ranges of the same inode, each
 * with its own nfs_direct_req, sharing the open context and the same
 * nfs_page cache invalidation. The port keeps upstream's shape -- an
 * ALLOCATEd file, a cache drop, then interleaved direct reads -- and adds
 * a check upstream does not make: every byte read must be zero, since
 * ALLOCATE'd space reads as a hole.
 *
 * Deviations: 64 extents rather than 1024, and the two readers run freely
 * rather than stepping through a barrier: a kthread and the test thread
 * interleave on their own, and the barrier is not what the property
 * depends on.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G391_ROOT	XFS_MNT "/g391"
#define G391_FILE	G391_ROOT "/391-testfile"

#define G391_EXTENT	8192			/* two 4k blocks */
#define G391_EXTENTS	64
#define G391_HALF	(G391_EXTENT / 2)

struct g391_reader {
	struct file		*f;
	loff_t			start;		/* offset within each extent */
	int			err;
	int			nonzero;
	struct completion	done;
};

static int g391_read_halves(void *arg)
{
	struct g391_reader *r = arg;
	loff_t off;
	u8 *buf;
	int i;

	buf = kmalloc(G391_HALF, GFP_KERNEL);
	if (!buf) {
		r->err = -ENOMEM;
		complete(&r->done);
		return 0;
	}

	for (off = (G391_EXTENTS - 1) * (loff_t)G391_EXTENT + r->start;
	     off >= 0; off -= G391_EXTENT) {
		loff_t pos = off;
		ssize_t n = xfs_direct_read(r->f, buf, G391_HALF, &pos);

		if (n != G391_HALF) {
			r->err = n < 0 ? (int)n : -EIO;
			break;
		}
		for (i = 0; i < G391_HALF; i++)
			if (buf[i]) {
				r->nonzero++;
				break;
			}
	}
	kfree(buf);
	complete(&r->done);
	return 0;
}

static void g391_remove_tree(void *unused)
{
	xfs_unlink(G391_FILE);
	xfs_rmdir_settled(G391_ROOT);
}

static void interleaved_direct_reads_of_one_extent(struct kunit *test)
{
	struct g391_reader first = { .start = 0 };
	struct g391_reader second = { .start = G391_HALF };
	struct task_struct *t;
	struct file *f, *rf;
	loff_t off;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G391_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g391_remove_tree, NULL),
			0);

	/* one ALLOCATE per extent, as upstream does */
	f = filp_open(G391_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	for (off = 0; off < (loff_t)G391_EXTENTS * G391_EXTENT;
	     off += G391_EXTENT)
		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_fallocate(f, 0, off, G391_EXTENT), 0,
				    "falloc at %lld failed", off);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_ASSERT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);
	filp_close(f, NULL);

	rf = filp_open(G391_FILE, O_RDONLY | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rf), "direct open: %ld",
			       PTR_ERR(rf));
	first.f = rf;
	second.f = rf;
	init_completion(&first.done);
	init_completion(&second.done);

	t = kthread_run(g391_read_halves, &first, "g391-reader");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));
	g391_read_halves(&second);
	wait_for_completion(&first.done);

	KUNIT_EXPECT_EQ_MSG(test, first.err, 0,
			    "the first half reader failed: %d", first.err);
	KUNIT_EXPECT_EQ_MSG(test, second.err, 0,
			    "the second half reader failed: %d", second.err);
	KUNIT_EXPECT_EQ_MSG(test, first.nonzero, 0,
			    "%d of the first reader's extents were not zero",
			    first.nonzero);
	KUNIT_EXPECT_EQ_MSG(test, second.nonzero, 0,
			    "%d of the second reader's extents were not zero",
			    second.nonzero);

	filp_close(rf, NULL);
}

static int g391_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g391_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g391_cases[] = {
	KUNIT_CASE_SLOW(interleaved_direct_reads_of_one_extent),
	{}
};

static struct kunit_suite g391_suite = {
	.name		= "xfstests/generic/391",
	.suite_init	= g391_suite_init,
	.suite_exit	= g391_suite_exit,
	.test_cases	= g391_cases,
};

kunit_test_suites(&g391_suite);

MODULE_DESCRIPTION("xfstests generic/391 over a loopback NFS mount");
MODULE_LICENSE("GPL");
