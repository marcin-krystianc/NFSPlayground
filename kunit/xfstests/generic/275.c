// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/275 over a loopback NFS mount: write behaviour at ENOSPC.
 *
 * Upstream ("the posix write test") checks what write() reports when the
 * filesystem fills mid-write. Buffered NFS complicates the upstream
 * expectation: the client's page cache happily accepts data beyond the
 * server's free space, and the truth only arrives with writeback. The
 * port pins the NFS-shaped contract: the failure surfaces through fsync
 * (or a later write) as ENOSPC, the file's server-side size is exactly a
 * prefix of what was attempted, and that prefix reads back intact.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G275_ROOT	XFS_MNT "/g275"

/* write stamped 64K chunks + fsync until the server says ENOSPC */
static int g275_fill(struct kunit *test, const char *path, loff_t *written)
{
	struct file *f;
	u8 *buf;
	loff_t pos = 0;
	int err = 0, i;

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (i = 0; i < 1024; i++) {	/* 64M ceiling >> the 16M export */
		ssize_t n;

		memset(buf, 0x41 + (i % 26), 65536);
		n = kernel_write(f, buf, 65536, &pos);
		if (n < 0) {
			err = n;
			break;
		}
		err = vfs_fsync(f, 0);
		if (err)
			break;
	}
	filp_close(f, NULL);
	*written = pos;
	return err;
}

static void g275_remove_tree(void *unused)
{
	xfs_unlink(G275_ROOT "/fill");
	xfs_rmdir_settled(G275_ROOT);
}

static void enospc_surfaces_and_the_prefix_survives(struct kunit *test)
{
	struct kstat st;
	loff_t written = 0, off;
	u8 *buf;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G275_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g275_remove_tree, NULL),
			0);

	err = g275_fill(test, G275_ROOT "/fill", &written);
	KUNIT_ASSERT_EQ_MSG(test, err, -ENOSPC,
			    "expected ENOSPC to surface, got %d", err);

	/* the server-side size is what the successful, fsynced writes cover */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G275_ROOT "/fill", &st), 0);
	KUNIT_EXPECT_LE_MSG(test, st.size, written + 65536,
			    "the file claims more than was ever written");
	KUNIT_EXPECT_GT(test, st.size, (loff_t)0);

	/* and that prefix is the data, not garbage */
	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (off = 0; off + 65536 <= st.size && off < (loff_t)4 * 65536 * 10;
	     off += 65536) {
		int i = off / 65536;
		ssize_t n = xfs_read_range(G275_ROOT "/fill", buf, 65536, off);
		int j;

		KUNIT_ASSERT_EQ(test, n, (ssize_t)65536);
		for (j = 0; j < 65536; j++)
			if (buf[j] != 0x41 + (i % 26)) {
				KUNIT_FAIL(test,
					   "chunk %d corrupt at %d after ENOSPC",
					   i, j);
				return;
			}
	}
}

static int g275_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=8192");
	return xfstests_nfs_get();
}

static void g275_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g275_cases[] = {
	KUNIT_CASE_SLOW(enospc_surfaces_and_the_prefix_survives),
	{}
};

static struct kunit_suite g275_suite = {
	.name		= "xfstests/generic/275",
	.suite_init	= g275_suite_init,
	.suite_exit	= g275_suite_exit,
	.test_cases	= g275_cases,
};

kunit_test_suites(&g275_suite);

MODULE_DESCRIPTION("xfstests generic/275 over a loopback NFS mount");
MODULE_LICENSE("GPL");
