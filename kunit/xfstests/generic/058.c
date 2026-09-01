// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/058 over a loopback NFS mount: insert range.
 *
 * Upstream exercises FALLOC_FL_INSERT_RANGE. Same story as generic/021:
 * no NFSv4.2 mapping, xfstests _notruns at _require_xfs_io_command
 * "finsert", and this port pins the EOPNOTSUPP contract plus data
 * integrity of the rejected call. Stands for generic/060, 061, 063 and
 * the insert half of 064.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G058_ROOT	XFS_MNT "/g058"

static void g058_remove_tree(void *unused)
{
	xfs_unlink(G058_ROOT "/f");
	xfs_rmdir(G058_ROOT);
}

static void insert_range_is_rejected_and_harmless(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	ssize_t n;
	int err, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G058_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g058_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 32768, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < 32768; i++)
		buf[i] = 0xC5;
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G058_ROOT "/f", buf, 32768), 0);

	f = filp_open(G058_ROOT "/f", O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	err = vfs_fallocate(f, FALLOC_FL_INSERT_RANGE, 4096, 4096);
	filp_close(f, NULL);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "INSERT_RANGE over NFS: expected EOPNOTSUPP, got %d",
			    err);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G058_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)32768,
			    "a rejected insert changed the file size");
	n = xfs_read_range(G058_ROOT "/f", buf, 32768, 0);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)32768);
	for (i = 0; i < 32768; i++)
		if (buf[i] != 0xC5) {
			KUNIT_FAIL(test, "byte %d changed after a rejected insert",
				   i);
			return;
		}
}

static int g058_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g058_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g058_cases[] = {
	KUNIT_CASE(insert_range_is_rejected_and_harmless),
	{}
};

static struct kunit_suite g058_suite = {
	.name		= "xfstests/generic/058",
	.suite_init	= g058_suite_init,
	.suite_exit	= g058_suite_exit,
	.test_cases	= g058_cases,
};

kunit_test_suites(&g058_suite);

MODULE_DESCRIPTION("xfstests generic/058 over a loopback NFS mount");
MODULE_LICENSE("GPL");
