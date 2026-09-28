// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/363 over a loopback NFS mount: fsx with EOF pollution checks.
 *
 * Upstream runs "fsx -q -S 0 -e 1 -N 100000" on TEST_DIR/junk: 100000
 * operations from seed 0 with EOF pollution on (-e 1). Before each
 * operation that extends the file, fsx writes garbage through a shared
 * mapping into the part of the EOF page past EOF (pollute_eofpage()); the
 * extending operation must zero that range, and later reads compare it
 * with fsx's copy of the file, which has zeroes there.
 *
 * Over NFS those bytes are in the client's page cache: the extending
 * write or truncate has to zero them there (on master,
 * nfs_truncate_last_folio()) before they can be read back or written to
 * the server.
 *
 * The port runs upstream's fsx as a userspace process inside the test
 * kernel (xfs_run_prog()), with upstream's arguments.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G363_ROOT	XFS_MNT "/g363"
#define G363_FILE	G363_ROOT "/junk"

static void g363_remove_tree(void *unused)
{
	xfs_unlink(G363_FILE);
	xfs_unlink(G363_FILE ".fsxlog");
	xfs_unlink(G363_FILE ".fsxgood");
	xfs_rmdir_settled(G363_ROOT);
}

/* common/rc's _run_fsx_on_file: rm -f the file, then run fsx on it */
static void g363_run_fsx(struct kunit *test, const char *const args[])
{
	int ret;

	xfs_unlink(G363_FILE);
	xfs_settle_fput();
	ret = xfs_run_prog(test, "fsx", args);
	KUNIT_EXPECT_EQ_MSG(test, ret, 0, "fsx exited with %d", ret);
}

static void fsx_eof_pollution(struct kunit *test)
{
	/* run_fsx "-q -S 0 -e 1 -N 100000" */
	const char *const args[] = {
		"-q", "-S", "0", "-e", "1", "-N", "100000", G363_FILE, NULL,
	};

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G363_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g363_remove_tree, NULL),
			0);
	g363_run_fsx(test, args);
}

static int g363_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g363_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g363_cases[] = {
	KUNIT_CASE_SLOW(fsx_eof_pollution),
	{}
};

static struct kunit_suite g363_suite = {
	.name		= "xfstests/generic/363",
	.suite_init	= g363_suite_init,
	.suite_exit	= g363_suite_exit,
	.test_cases	= g363_cases,
};

kunit_test_suites(&g363_suite);

MODULE_DESCRIPTION("xfstests generic/363 over a loopback NFS mount");
MODULE_LICENSE("GPL");
