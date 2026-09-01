// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/308 over a loopback NFS mount: writes at extreme offsets.
 *
 * Upstream is a regression for max-file-size bookkeeping: writes near
 * the filesystem's limits. Over NFS the port writes one byte at a 1 TB
 * offset (sparse; the tmpfs export accounts pages, not logical size),
 * verifies the size, the byte, and the hole before it, then pins the
 * negative-offset EINVAL. Found while porting: without O_LARGEFILE the
 * 2 GiB MAX_NON_LFS limit still applies to in-kernel opens.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G308_ROOT	XFS_MNT "/g308"

#define G308_FAR	((loff_t)1 << 40)

static void g308_remove_tree(void *unused)
{
	xfs_unlink(G308_ROOT "/f");
	xfs_rmdir(G308_ROOT);
}

static void far_offsets_round_trip(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos;
	u8 b = 0x5C, rd[16];
	ssize_t n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G308_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g308_remove_tree, NULL),
			0);

	/*
	 * O_LARGEFILE by hand: userspace opens get it via
	 * force_o_largefile(), in-kernel filp_open does not, and without it
	 * generic_write_checks caps the file at MAX_NON_LFS (2 GiB) with
	 * EFBIG -- which is itself the class of limit this test is about.
	 */
	f = filp_open(G308_ROOT "/f",
		      O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	pos = G308_FAR;
	n = kernel_write(f, &b, 1, &pos);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)1,
			    "1-byte write at 1TB failed: %zd", n);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G308_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, G308_FAR + 1);

	pos = G308_FAR;
	n = kernel_read(f, rd, 1, &pos);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)1);
	KUNIT_EXPECT_EQ(test, rd[0], (u8)0x5C);

	/* the terabyte before it is a hole and reads zero */
	pos = G308_FAR - 8;
	n = kernel_read(f, rd, 8, &pos);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)8);
	KUNIT_EXPECT_EQ(test, rd[0] | rd[1] | rd[2] | rd[3] |
			      rd[4] | rd[5] | rd[6] | rd[7], 0);

	/* negative offsets are rejected before any RPC */
	pos = -4096;
	n = kernel_write(f, &b, 1, &pos);
	KUNIT_EXPECT_EQ(test, n, (ssize_t)-EINVAL);

	filp_close(f, NULL);
}

static int g308_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g308_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g308_cases[] = {
	KUNIT_CASE(far_offsets_round_trip),
	{}
};

static struct kunit_suite g308_suite = {
	.name		= "xfstests/generic/308",
	.suite_init	= g308_suite_init,
	.suite_exit	= g308_suite_exit,
	.test_cases	= g308_cases,
};

kunit_test_suites(&g308_suite);

MODULE_DESCRIPTION("xfstests generic/308 over a loopback NFS mount");
MODULE_LICENSE("GPL");
