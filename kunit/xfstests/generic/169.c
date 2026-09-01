// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/169 over a loopback NFS mount: appends with fsync and exact sizes.
 *
 * Upstream appends 5k three times to a new file, fsyncing between, and
 * checks the reported size after every step -- a regression for size
 * updates racing writeback. Sizes here are checked against the server
 * (FORCE_SYNC stat) after every append+fsync, then the file is reopened
 * O_TRUNC and the zero size must be visible too.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G169_ROOT	XFS_MNT "/g169"

static void g169_remove_tree(void *unused)
{
	xfs_unlink(G169_ROOT "/f");
	xfs_rmdir(G169_ROOT);
}

static void synced_appends_report_exact_sizes(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	int step;
	u32 j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G169_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g169_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 5120, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G169_ROOT "/f", O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (step = 1; step <= 3; step++) {
		loff_t pos = (step - 1) * 5120;

		for (j = 0; j < 5120; j++)
			buf[j] = (u8)(step * 40 + (j & 31));
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 5120, &pos),
				(ssize_t)5120);
		KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G169_ROOT "/f", &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)step * 5120,
				    "after synced append %d: size %lld", step,
				    st.size);
	}
	filp_close(f, NULL);

	/* verify all three stripes landed */
	for (step = 1; step <= 3; step++) {
		ssize_t n = xfs_read_range(G169_ROOT "/f", buf, 5120,
					   (step - 1) * 5120);

		KUNIT_ASSERT_EQ(test, n, (ssize_t)5120);
		for (j = 0; j < 5120; j++)
			if (buf[j] != (u8)(step * 40 + (j & 31))) {
				KUNIT_FAIL(test, "stripe %d byte %u", step, j);
				return;
			}
	}

	/* O_TRUNC: the zero size must be server-visible immediately */
	f = filp_open(G169_ROOT "/f", O_WRONLY | O_TRUNC, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G169_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)0);
}

static int g169_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g169_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g169_cases[] = {
	KUNIT_CASE(synced_appends_report_exact_sizes),
	{}
};

static struct kunit_suite g169_suite = {
	.name		= "xfstests/generic/169",
	.suite_init	= g169_suite_init,
	.suite_exit	= g169_suite_exit,
	.test_cases	= g169_cases,
};

kunit_test_suites(&g169_suite);

MODULE_DESCRIPTION("xfstests generic/169 over a loopback NFS mount");
MODULE_LICENSE("GPL");
