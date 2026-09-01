// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/021 over a loopback NFS mount: collapse range.
 *
 * Upstream exercises FALLOC_FL_COLLAPSE_RANGE and verifies the data shift.
 * On NFS, xfstests _notruns at _require_xfs_io_command "fcollapse":
 * nfs42_fallocate() accepts only ALLOCATE (0) and PUNCH_HOLE|KEEP_SIZE, so
 * collapse has no protocol mapping. This port pins that contract -- the
 * exact errno, and that the failed call left the data untouched -- and
 * stands for the whole collapse family (generic/016, 017, 022, 031, 064
 * and 072 share the same notrun reason).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G021_ROOT	XFS_MNT "/g021"

static void g021_remove_tree(void *unused)
{
	xfs_unlink(G021_ROOT "/f");
	xfs_rmdir(G021_ROOT);
}

static void collapse_range_is_rejected_and_harmless(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	ssize_t n;
	int err, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G021_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g021_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < 65536; i++)
		buf[i] = i / 4096;	/* one recognisable byte per 4K block */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G021_ROOT "/f", buf, 65536), 0);

	f = filp_open(G021_ROOT "/f", O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	err = vfs_fallocate(f, FALLOC_FL_COLLAPSE_RANGE, 8192, 8192);
	filp_close(f, NULL);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "COLLAPSE_RANGE over NFS: expected EOPNOTSUPP, got %d",
			    err);

	/* the rejected call must not have moved a byte or the size */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G021_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)65536);
	n = xfs_read_range(G021_ROOT "/f", buf, 65536, 0);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)65536);
	for (i = 0; i < 65536; i++)
		if (buf[i] != i / 4096) {
			KUNIT_FAIL(test, "byte %d changed after a rejected collapse",
				   i);
			return;
		}
}

static int g021_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g021_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g021_cases[] = {
	KUNIT_CASE(collapse_range_is_rejected_and_harmless),
	{}
};

static struct kunit_suite g021_suite = {
	.name		= "xfstests/generic/021",
	.suite_init	= g021_suite_init,
	.suite_exit	= g021_suite_exit,
	.test_cases	= g021_cases,
};

kunit_test_suites(&g021_suite);

MODULE_DESCRIPTION("xfstests generic/021 over a loopback NFS mount");
MODULE_LICENSE("GPL");
