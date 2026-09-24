// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/763 over a loopback NFS mount: a zero-byte write.
 *
 * Upstream is one pwrite of length zero, which must report "wrote 0/0
 * bytes" rather than failing. exfat regressed it into -EFAULT (commit
 * dda0407a2026, "exfat: short-circuit zero-byte writes in
 * exfat_file_write_iter").
 *
 * Over NFS a zero-length write must not become a zero-length WRITE RPC
 * either, and it must not create or extend anything: the port checks the
 * return value, that the file is still empty on both the client and the
 * server, and that the same holds on the direct path, where a zero-length
 * iterator is the case that reaches nfs_direct_write()'s early return.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G763_ROOT	XFS_MNT "/g763"
#define G763_FILE	G763_ROOT "/testfile.763"
#define G763_SERVER	XFS_EXPORT "/g763/testfile.763"

static void g763_remove_tree(void *unused)
{
	xfs_unlink(G763_FILE);
	xfs_rmdir_settled(G763_ROOT);
}

static void a_zero_byte_write_succeeds_and_changes_nothing(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G763_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g763_remove_tree, NULL),
			0);

	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G763_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	KUNIT_EXPECT_EQ_MSG(test, kernel_write(f, buf, 0, &pos), 0L,
			    "a zero-byte buffered write did not return 0");
	KUNIT_EXPECT_EQ(test, pos, 0LL);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G763_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL, "the file is %lld bytes",
			    st.size);

	/* and the same on the direct path */
	f = filp_open(G763_FILE, O_RDWR | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct open: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_EXPECT_EQ_MSG(test, xfs_direct_write(f, buf, 0, &pos), 0L,
			    "a zero-byte direct write did not return 0");
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G763_SERVER, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
			    "the server holds %lld bytes", st.size);
}

static int g763_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g763_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g763_cases[] = {
	KUNIT_CASE(a_zero_byte_write_succeeds_and_changes_nothing),
	{}
};

static struct kunit_suite g763_suite = {
	.name		= "xfstests/generic/763",
	.suite_init	= g763_suite_init,
	.suite_exit	= g763_suite_exit,
	.test_cases	= g763_cases,
};

kunit_test_suites(&g763_suite);

MODULE_DESCRIPTION("xfstests generic/763 over a loopback NFS mount");
MODULE_LICENSE("GPL");
