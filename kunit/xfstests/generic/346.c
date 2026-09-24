// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/346 over a loopback NFS mount: a mapped write and a pwrite into the same pages.
 *
 * Upstream runs src/holetest -f -w on 1, 16 and 256 MiB: thread 0 marks
 * its slot in every page with pwrite(2), thread 1 through the mapping.
 * Every page must end up holding both ids, so the two paths into the same
 * page have to agree.
 *
 * Over NFS they are genuinely different paths: the mapped write dirties
 * the page through nfs_vm_page_mkwrite() and records its range, while the
 * pwrite goes through nfs_write_begin()/nfs_write_end() on the same page.
 * If either writes back the whole page rather than its own range, it
 * overwrites the other's slot.
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

#define G346_DIR	"g346"
#define G346_FILE	G346_DIR "/testfile"
#define G346_MIB	(1024 * 1024LL)
/* room for the largest file sized with posix_fallocate or a zero fill */
#define G346_EXPORT	"size=335544320,nr_inodes=32768"

static void g346_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G346_FILE);
	xfs_rmdir_settled(XFS_MNT "/" G346_DIR);
}

static void g346_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G346_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g346_remove_tree,
						  NULL), 0);
}

/* holetest -f -w $SCRATCH_MNT/testfile 1 */
static void holetest_w_1m(struct kunit *test)
{
	g346_setup(test);
	xfs_holetest(test, G346_FILE, 1 * G346_MIB, XFS_HOLETEST_WRITE);
}

/* holetest -f -w $SCRATCH_MNT/testfile 16 */
static void holetest_w_16m(struct kunit *test)
{
	g346_setup(test);
	xfs_holetest(test, G346_FILE, 16 * G346_MIB, XFS_HOLETEST_WRITE);
}

/* holetest -f -w $SCRATCH_MNT/testfile 256 */
static void holetest_w_256m(struct kunit *test)
{
	g346_setup(test);
	xfs_holetest(test, G346_FILE, 256 * G346_MIB, XFS_HOLETEST_WRITE);
}


static int g346_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts(G346_EXPORT);
	return xfstests_nfs_get();
}

static void g346_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g346_cases[] = {
	KUNIT_CASE_SLOW(holetest_w_1m),
	KUNIT_CASE_SLOW(holetest_w_16m),
	KUNIT_CASE_SLOW(holetest_w_256m),
	{}
};

static struct kunit_suite g346_suite = {
	.name		= "xfstests/generic/346",
	.suite_init	= g346_suite_init,
	.suite_exit	= g346_suite_exit,
	.test_cases	= g346_cases,
};

kunit_test_suites(&g346_suite);

MODULE_DESCRIPTION("xfstests generic/346 over a loopback NFS mount");
MODULE_LICENSE("GPL");
