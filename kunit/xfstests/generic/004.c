// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/004 over a loopback NFS mount: O_TMPFILE.
 *
 * The original creates an O_TMPFILE, writes through it, and links it back
 * into the namespace with flink. On NFS, xfstests never gets that far:
 * `_require_xfs_io_command "-T"` probes O_TMPFILE and _notruns, because
 * the NFS protocol has no anonymous-file create and fs/nfs defines no
 * ->tmpfile operation.
 *
 * This port is the honest mirror of that notrun: instead of skipping
 * silently, it pins the exact contract -- opening with O_TMPFILE on the
 * NFS mount must fail with EOPNOTSUPP (the VFS's answer when an fs lacks
 * ->tmpfile), and must not leave anything behind. If NFS ever grows
 * O_TMPFILE support, this fails loudly and the real generic/004 logic
 * becomes portable.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G004_ROOT	XFS_MNT "/g004"

static void g004_remove_tree(void *unused)
{
	xfs_rmdir(G004_ROOT);
}

static void o_tmpfile_is_rejected_with_eopnotsupp(struct kunit *test)
{
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G004_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g004_remove_tree,
						  NULL), 0);

	f = filp_open(G004_ROOT, O_TMPFILE | O_RDWR, 0600);
	KUNIT_ASSERT_TRUE_MSG(test, IS_ERR(f),
			      "O_TMPFILE unexpectedly succeeded over NFS");
	KUNIT_EXPECT_EQ_MSG(test, PTR_ERR(f), (long)-EOPNOTSUPP,
			    "expected EOPNOTSUPP for O_TMPFILE, got %ld",
			    PTR_ERR(f));

	/* the failed open must leave the directory empty */
	KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir(G004_ROOT), 0,
			    "the failed O_TMPFILE left something in the directory");
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G004_ROOT), 0);	/* for the action */
}

static int g004_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g004_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g004_cases[] = {
	KUNIT_CASE(o_tmpfile_is_rejected_with_eopnotsupp),
	{}
};

static struct kunit_suite g004_suite = {
	.name		= "xfstests/generic/004",
	.suite_init	= g004_suite_init,
	.suite_exit	= g004_suite_exit,
	.test_cases	= g004_cases,
};

kunit_test_suites(&g004_suite);

MODULE_DESCRIPTION("xfstests generic/004 over a loopback NFS mount");
MODULE_LICENSE("GPL");
