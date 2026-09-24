// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/344 over a loopback NFS mount: holetest with the pages prefaulted.
 *
 * Upstream runs src/holetest -f -r on 16 and 256 MiB, then -f -r -w on
 * the same sizes. -r reads every page of the mapping before the threads
 * start, so the pages are present and clean and the two writers have to
 * dirty them without losing each other's slot; -w has thread 0 write its
 * id with pwrite(2) instead of through the mapping.
 *
 * Over NFS a prefaulted page is one the client has already read from the
 * server (or zero-filled for a hole), so the write path takes the
 * "partial write to an up-to-date page" branch of nfs_write_begin()
 * rather than the brand-new-page one.
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

#define G344_DIR	"g344"
#define G344_FILE	G344_DIR "/testfile"
#define G344_MIB	(1024 * 1024LL)
/* room for the largest file sized with posix_fallocate or a zero fill */
#define G344_EXPORT	"size=335544320,nr_inodes=32768"

static void g344_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G344_FILE);
	xfs_rmdir_settled(XFS_MNT "/" G344_DIR);
}

static void g344_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G344_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g344_remove_tree,
						  NULL), 0);
}

/* holetest -f -r $SCRATCH_MNT/testfile 16 */
static void holetest_r_16m(struct kunit *test)
{
	g344_setup(test);
	xfs_holetest(test, G344_FILE, 16 * G344_MIB, XFS_HOLETEST_PREFAULT);
}

/* holetest -f -r $SCRATCH_MNT/testfile 256 */
static void holetest_r_256m(struct kunit *test)
{
	g344_setup(test);
	xfs_holetest(test, G344_FILE, 256 * G344_MIB, XFS_HOLETEST_PREFAULT);
}

/* holetest -f -r -w $SCRATCH_MNT/testfile 16 */
static void holetest_r_w_16m(struct kunit *test)
{
	g344_setup(test);
	xfs_holetest(test, G344_FILE, 16 * G344_MIB, XFS_HOLETEST_PREFAULT | XFS_HOLETEST_WRITE);
}

/* holetest -f -r -w $SCRATCH_MNT/testfile 256 */
static void holetest_r_w_256m(struct kunit *test)
{
	g344_setup(test);
	xfs_holetest(test, G344_FILE, 256 * G344_MIB, XFS_HOLETEST_PREFAULT | XFS_HOLETEST_WRITE);
}


static int g344_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G344_EXPORT);
	return xfstests_nfs_get();
}

static void g344_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g344_cases[] = {
	KUNIT_CASE_SLOW(holetest_r_16m),
	KUNIT_CASE_SLOW(holetest_r_256m),
	KUNIT_CASE_SLOW(holetest_r_w_16m),
	KUNIT_CASE_SLOW(holetest_r_w_256m),
	{}
};

static struct kunit_suite g344_suite = {
	.name		= "xfstests/generic/344",
	.suite_init	= g344_suite_init,
	.suite_exit	= g344_suite_exit,
	.test_cases	= g344_cases,
};

kunit_test_suites(&g344_suite);

MODULE_DESCRIPTION("xfstests generic/344 over a loopback NFS mount");
MODULE_LICENSE("GPL");
