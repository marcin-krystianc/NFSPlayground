// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/354 over a loopback NFS mount: two markers in one MAP_PRIVATE mapping.
 *
 * Upstream runs src/holetest -f -p on 16 and 256 MiB, then -f -p -F on
 * the same sizes. -p maps the file MAP_PRIVATE, so the threads' writes
 * land in copy-on-write pages: both must be visible through the mapping,
 * and a read-only private mapping made afterwards must see none of them
 * in the file. -F uses processes instead of threads, so each child has
 * its own copy and sees only its own mark.
 *
 * Over NFS the COW fault has to read the page from the server (or
 * zero-fill it for a hole) and then detach it from the file's mapping, so
 * what this checks is that a private fault neither dirties the file's
 * own page nor sends a WRITE.
 *
 * src/holetest runs each size three times, the file sized a different way
 * each time: ftruncate plus an explicit zero fill through a shared
 * mapping, posix_fallocate, and plain ftruncate. Two threads then write
 * their own id a quarter and three quarters of the way into every page,
 * and every page must end up holding both ids. xfs_holetest() is holetest
 * with its threads as kthreads borrowing this thread's mm; it also reads
 * the ids back from the server's copy of the file after it is closed.
 *
 * Deviation: the -F runs are not ported. They need two address spaces,
 * and the markers here are kthreads borrowing one mm.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G354_DIR	"g354"
#define G354_FILE	G354_DIR "/testfile"
#define G354_MIB	(1024 * 1024LL)
/* room for the largest file sized with posix_fallocate or a zero fill */
#define G354_EXPORT	"size=335544320,nr_inodes=32768"

static void g354_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G354_FILE);
	xfs_rmdir_settled(XFS_MNT "/" G354_DIR);
}

static void g354_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G354_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g354_remove_tree,
						  NULL), 0);
}

/* holetest -f -p $SCRATCH_MNT/testfile 16 */
static void holetest_p_16m(struct kunit *test)
{
	g354_setup(test);
	xfs_holetest(test, G354_FILE, 16 * G354_MIB, XFS_HOLETEST_PRIVATE);
}

/* holetest -f -p $SCRATCH_MNT/testfile 256 */
static void holetest_p_256m(struct kunit *test)
{
	g354_setup(test);
	xfs_holetest(test, G354_FILE, 256 * G354_MIB, XFS_HOLETEST_PRIVATE);
}


static int g354_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G354_EXPORT);
	return xfstests_nfs_get();
}

static void g354_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g354_cases[] = {
	KUNIT_CASE_SLOW(holetest_p_16m),
	KUNIT_CASE_SLOW(holetest_p_256m),
	{}
};

static struct kunit_suite g354_suite = {
	.name		= "xfstests/generic/354",
	.suite_init	= g354_suite_init,
	.suite_exit	= g354_suite_exit,
	.test_cases	= g354_cases,
};

kunit_test_suites(&g354_suite);

MODULE_DESCRIPTION("xfstests generic/354 over a loopback NFS mount");
MODULE_LICENSE("GPL");
