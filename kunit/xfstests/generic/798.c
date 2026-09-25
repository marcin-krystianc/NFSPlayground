// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/798 over a loopback NFS mount: cachestat() after a
 * buffered write and after an fsync.
 *
 * Upstream, for files of 1, 2, 4, 8 and 16 pages: one xfs_io writes the
 * file page by page, a second runs cachestat over it, a third fsyncs and
 * runs cachestat again. 798.out expects every page cached and dirty after
 * the write, and cached and clean after the fsync.
 *
 * Each xfs_io is a separate open and close, and closing an NFS file that
 * was open for writing writes its dirty pages back (nfs4_file_flush()).
 * The port keeps upstream's opens and closes and upstream's expectations.
 *
 * Deviations: cachestat(2)'s body, filemap_cachestat(), is called on the
 * file's mapping; run-nfs-kunit.sh un-statics it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>
#include <linux/mman.h>

#include "xfstests_nfs_fixture.h"

/* mm/filemap.c, un-staticed by run-nfs-kunit.sh */
void filemap_cachestat(struct address_space *mapping, pgoff_t first_index,
		       pgoff_t last_index, struct cachestat *cs);

#define G798_ROOT	XFS_MNT "/g798"
#define G798_FILE	G798_ROOT "/foobar"

static void g798_remove_tree(void *unused)
{
	xfs_unlink(G798_FILE);
	xfs_rmdir_settled(G798_ROOT);
}

/* xfs_io -c "cachestat 0 $size" (optionally after -c fsync) */
static void g798_cachestat(struct kunit *test, int pages, bool fsync,
			   u64 want_dirty)
{
	struct cachestat cs = {};
	struct file *f;

	f = filp_open(G798_FILE, O_RDWR, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	if (fsync)
		KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filemap_cachestat(f->f_mapping, 0, pages - 1, &cs);
	filp_close(f, NULL);

	kunit_info(test,
		   "%d pages%s: Cached: %llu, Dirty: %llu, Writeback: %llu, Evicted: %llu, Recently Evicted: %llu\n",
		   pages, fsync ? " after fsync" : "", cs.nr_cache,
		   cs.nr_dirty, cs.nr_writeback, cs.nr_evicted,
		   cs.nr_recently_evicted);
	KUNIT_EXPECT_EQ_MSG(test, cs.nr_cache, (u64)pages, "%d pages%s",
			    pages, fsync ? " after fsync" : "");
	KUNIT_EXPECT_EQ_MSG(test, cs.nr_dirty, want_dirty, "%d pages%s",
			    pages, fsync ? " after fsync" : "");
	KUNIT_EXPECT_EQ_MSG(test, cs.nr_writeback, 0ULL, "%d pages%s",
			    pages, fsync ? " after fsync" : "");
	KUNIT_EXPECT_EQ(test, cs.nr_evicted, 0ULL);
	KUNIT_EXPECT_EQ(test, cs.nr_recently_evicted, 0ULL);
}

static void cachestat_after_write_and_fsync(struct kunit *test)
{
	u8 *buf;
	int pages, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G798_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g798_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, PAGE_SIZE);

	for (pages = 1; pages <= 16; pages *= 2) {
		struct file *f;

		/* rm -f; xfs_io -f -c "pwrite -b $pagesize 0 $size" */
		xfs_unlink(G798_FILE);
		xfs_settle_fput();
		f = filp_open(G798_FILE, O_RDWR | O_CREAT, 0600);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld",
				       PTR_ERR(f));
		for (i = 0; i < pages; i++) {
			loff_t pos = (loff_t)i * PAGE_SIZE;

			KUNIT_ASSERT_EQ(test,
					kernel_write(f, buf, PAGE_SIZE, &pos),
					(ssize_t)PAGE_SIZE);
		}
		filp_close(f, NULL);

		g798_cachestat(test, pages, false, pages);
		g798_cachestat(test, pages, true, 0);
	}
}

static int g798_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g798_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g798_cases[] = {
	KUNIT_CASE(cachestat_after_write_and_fsync),
	{}
};

static struct kunit_suite g798_suite = {
	.name		= "xfstests/generic/798",
	.suite_init	= g798_suite_init,
	.suite_exit	= g798_suite_exit,
	.test_cases	= g798_cases,
};

kunit_test_suites(&g798_suite);

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);
MODULE_DESCRIPTION("xfstests generic/798 over a loopback NFS mount");
MODULE_LICENSE("GPL");
