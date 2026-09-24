// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/340 over a loopback NFS mount: mmap writes from racing threads.
 *
 * Upstream ("Test mmap writing races from racing threads") runs
 * src/holetest on 1, 16 and 256 MiB files. Each page is a hole until one
 * of the two threads faults it in, so the race is over who creates the
 * page; if the fault path loses the other thread's write, its slot reads
 * back as zero.
 *
 * Over NFS that first fault is nfs_vm_page_mkwrite() on a page with no
 * server data behind it, and the two writes have to end up in the same
 * dirty range rather than as two competing versions of the page.
 *
 * src/holetest runs each size three times, the file sized a different way
 * each time: ftruncate plus an explicit zero fill through a shared
 * mapping, posix_fallocate, and plain ftruncate. Two threads then write
 * their own id a quarter and three quarters of the way into every page,
 * and every page must end up holding both ids. xfs_holetest() is holetest
 * with its threads as kthreads borrowing this thread's mm; it also reads
 * the ids back from the server's copy of the file after it is closed.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G340_DIR	"g340"
#define G340_FILE	G340_DIR "/testfile"
#define G340_MIB	(1024 * 1024LL)
/* room for the largest file sized with posix_fallocate or a zero fill */
#define G340_EXPORT	"size=335544320,nr_inodes=32768"

static void g340_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G340_FILE);
	xfs_rmdir_settled(XFS_MNT "/" G340_DIR);
}

static void g340_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G340_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g340_remove_tree,
						  NULL), 0);
}

/* holetest  $SCRATCH_MNT/testfile 1 */
static void holetest_1m(struct kunit *test)
{
	g340_setup(test);
	xfs_holetest(test, G340_FILE, 1 * G340_MIB, 0);
}

/* holetest  $SCRATCH_MNT/testfile 16 */
static void holetest_16m(struct kunit *test)
{
	g340_setup(test);
	xfs_holetest(test, G340_FILE, 16 * G340_MIB, 0);
}

/* holetest  $SCRATCH_MNT/testfile 256 */
static void holetest_256m(struct kunit *test)
{
	g340_setup(test);
	xfs_holetest(test, G340_FILE, 256 * G340_MIB, 0);
}


static int g340_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G340_EXPORT);
	return xfstests_nfs_get();
}

static void g340_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g340_cases[] = {
	KUNIT_CASE_SLOW(holetest_1m),
	KUNIT_CASE_SLOW(holetest_16m),
	KUNIT_CASE_SLOW(holetest_256m),
	{}
};

static struct kunit_suite g340_suite = {
	.name		= "xfstests/generic/340",
	.suite_init	= g340_suite_init,
	.suite_exit	= g340_suite_exit,
	.test_cases	= g340_cases,
};

kunit_test_suites(&g340_suite);

MODULE_DESCRIPTION("xfstests generic/340 over a loopback NFS mount");
MODULE_LICENSE("GPL");
