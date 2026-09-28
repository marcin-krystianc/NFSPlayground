// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/091 over a loopback NFS mount: fsx with O_DIRECT.
 *
 * Upstream runs ltp/fsx six times on TEST_DIR/junk, 10000 operations each
 * on a file of up to 500000 bytes, with O_DIRECT (-Z) and mapped reads and
 * writes disabled (-R -W; the last run allows mapped reads). Reads are
 * aligned to the page size (-r PSIZE), truncates and writes to the minimum
 * direct-I/O alignment (-t BSIZE -w BSIZE), and -o bounds the operation
 * size. fsx checks every read against its own copy of the file and exits
 * non-zero on the first mismatch.
 *
 * Over NFS the reads and writes are nfs_file_direct_read() and
 * nfs_file_direct_write(), mixed with buffered truncates and extends.
 *
 * The port runs upstream's fsx, built static by run-nfs-kunit.sh, as a
 * userspace process inside the test kernel (xfs_run_prog()), with
 * upstream's arguments. PSIZE is src/feature -s, the page size; BSIZE is
 * src/min_dio_alignment, which on NFS falls back to the page size too
 * (O_TMPFILE fails and TEST_DEV is not a block device).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G091_ROOT	XFS_MNT "/g091"
#define G091_FILE	G091_ROOT "/junk"

static void g091_remove_tree(void *unused)
{
	xfs_unlink(G091_FILE);
	xfs_unlink(G091_FILE ".fsxlog");
	xfs_unlink(G091_FILE ".fsxgood");
	xfs_rmdir_settled(G091_ROOT);
}

/* common/rc's _run_fsx_on_file: rm -f the file, then run fsx on it */
static void g091_run_fsx(struct kunit *test, const char *const args[])
{
	int ret;

	xfs_unlink(G091_FILE);
	xfs_settle_fput();
	ret = xfs_run_prog(test, "fsx", args);
	KUNIT_EXPECT_EQ_MSG(test, ret, 0, "fsx exited with %d", ret);
}

#define G091_PSIZE	"4096"

static void fsx_odirect_runs(struct kunit *test)
{
	static const char *const oplen[] = {
		NULL, "8192", "32768", "8192", "32768", "128000",
	};
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, PAGE_SIZE, 4096UL);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G091_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g091_remove_tree, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(oplen); i++) {
		/* run_fsx -N 10000 [-o N] -l 500000 -r PSIZE -t BSIZE -w BSIZE -Z [-R] -W */
		const char *args[20];
		int a = 0;

		args[a++] = "-N";
		args[a++] = "10000";
		if (oplen[i]) {
			args[a++] = "-o";
			args[a++] = oplen[i];
		}
		args[a++] = "-l";
		args[a++] = "500000";
		args[a++] = "-r";
		args[a++] = G091_PSIZE;
		args[a++] = "-t";
		args[a++] = G091_PSIZE;
		args[a++] = "-w";
		args[a++] = G091_PSIZE;
		args[a++] = "-Z";
		/* the last run leaves mapped reads on */
		if (i != ARRAY_SIZE(oplen) - 1)
			args[a++] = "-R";
		args[a++] = "-W";
		args[a++] = G091_FILE;
		args[a] = NULL;
		g091_run_fsx(test, args);
	}
}

static int g091_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g091_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g091_cases[] = {
	KUNIT_CASE_SLOW(fsx_odirect_runs),
	{}
};

static struct kunit_suite g091_suite = {
	.name		= "xfstests/generic/091",
	.suite_init	= g091_suite_init,
	.suite_exit	= g091_suite_exit,
	.test_cases	= g091_cases,
};

kunit_test_suites(&g091_suite);

MODULE_DESCRIPTION("xfstests generic/091 over a loopback NFS mount");
MODULE_LICENSE("GPL");
