// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/263 over a loopback NFS mount: fsx with O_DIRECT and mmap.
 *
 * Upstream runs ltp/fsx twice on TEST_DIR/junk: 10000 operations on a file
 * of up to 500000 bytes with O_DIRECT (-Z) and mapped reads and writes
 * left on, with operations of up to 8192 and then up to 128000 bytes. Reads
 * are page aligned (-r PSIZE), truncates and writes aligned to the minimum
 * direct-I/O alignment (-t BSIZE -w BSIZE). fsx checks every read against
 * its own copy of the file and exits non-zero on the first mismatch.
 *
 * Over NFS this mixes nfs_file_direct_read()/nfs_file_direct_write() with
 * mapped access through the page cache on the same file.
 *
 * The port runs upstream's fsx as a userspace process inside the test
 * kernel (xfs_run_prog()), with upstream's arguments. PSIZE and BSIZE are
 * both the page size on NFS, as in the generic/091 port.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G263_ROOT	XFS_MNT "/g263"
#define G263_FILE	G263_ROOT "/junk"

static void g263_remove_tree(void *unused)
{
	xfs_unlink(G263_FILE);
	xfs_unlink(G263_FILE ".fsxlog");
	xfs_unlink(G263_FILE ".fsxgood");
	xfs_rmdir_settled(G263_ROOT);
}

/* common/rc's _run_fsx_on_file: rm -f the file, then run fsx on it */
static void g263_run_fsx(struct kunit *test, const char *const args[])
{
	int ret;

	xfs_unlink(G263_FILE);
	xfs_settle_fput();
	ret = xfs_run_prog(test, "fsx", args);
	KUNIT_EXPECT_EQ_MSG(test, ret, 0, "fsx exited with %d", ret);
}

#define G263_PSIZE	"4096"

static void fsx_odirect_with_mmap(struct kunit *test)
{
	static const char *const oplen[] = { "8192", "128000" };
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, PAGE_SIZE, 4096UL);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G263_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g263_remove_tree, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(oplen); i++) {
		/* run_fsx -N 10000 -o N -l 500000 -r PSIZE -t BSIZE -w BSIZE -Z */
		const char *const args[] = {
			"-N", "10000", "-o", oplen[i], "-l", "500000",
			"-r", G263_PSIZE, "-t", G263_PSIZE, "-w", G263_PSIZE,
			"-Z", G263_FILE, NULL,
		};

		g263_run_fsx(test, args);
	}
}

static int g263_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g263_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g263_cases[] = {
	KUNIT_CASE_SLOW(fsx_odirect_with_mmap),
	{}
};

static struct kunit_suite g263_suite = {
	.name		= "xfstests/generic/263",
	.suite_init	= g263_suite_init,
	.suite_exit	= g263_suite_exit,
	.test_cases	= g263_cases,
};

kunit_test_suites(&g263_suite);

MODULE_DESCRIPTION("xfstests generic/263 over a loopback NFS mount");
MODULE_LICENSE("GPL");
