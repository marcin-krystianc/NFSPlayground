// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/393 over a loopback NFS mount: truncate to zero, then
 * back up.
 *
 * Upstream writes 40 bytes of 0x58, fsyncs, truncates to 0 and then up
 * again -- once to 50 bytes and once to 4096 -- and dumps the result
 * before and after cycling the mount. It was written for ext4/f2fs inline
 * data, where the bytes live inside the inode and a truncate has to clear
 * both the inode copy and the cached page, but the property it checks is
 * general: after truncating to zero, the re-extended file reads as a hole
 * everywhere, with nothing of the old content left.
 *
 * Over NFS the two truncates are two SETATTRs, and the client has to
 * drop the page it still holds from the write (nfs_vmtruncate() ->
 * truncate_pagecache()) rather than serve the old 0x58s back from it.
 * The 50-byte case leaves the client with a partial first page it must
 * treat as a hole; the 4096-byte case is the page-aligned one.
 *
 * Deviations: the mount cycle is replaced by the server's own copy read
 * through the tmpfs export, and od's hexdump by a byte-exact check.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G393_ROOT	XFS_MNT "/g393"
#define G393_FILE	G393_ROOT "/testfile"
#define G393_SERVER	XFS_EXPORT "/g393/testfile"

#define G393_WRITTEN	40
#define G393_MAX	4096

static void g393_remove_tree(void *unused)
{
	xfs_unlink(G393_FILE);
	xfs_rmdir_settled(G393_ROOT);
}

static void g393_all_zero(struct kunit *test, const char *path, loff_t size,
			  const char *which, u8 *buf)
{
	struct kstat st;
	ssize_t n;
	loff_t i;

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, size, "%s: size is %lld", which,
			    st.size);

	memset(buf, 0xff, G393_MAX);
	n = xfs_read_range(path, buf, size, 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)size, "%s: read returned %zd",
			    which, n);
	for (i = 0; i < size; i++)
		if (buf[i]) {
			KUNIT_FAIL(test, "%s: byte %lld is %02x, expected 00",
				   which, i, buf[i]);
			return;
		}
}

static void g393_one_round(struct kunit *test, loff_t up, u8 *buf)
{
	struct file *f;
	loff_t pos = 0;

	xfs_unlink(G393_FILE);

	memset(buf, 0x58, G393_WRITTEN);
	f = filp_open(G393_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G393_WRITTEN, &pos),
			(ssize_t)G393_WRITTEN);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ_MSG(test, xfs_truncate(G393_FILE, 0), 0,
			    "truncate to 0 failed");
	KUNIT_ASSERT_EQ_MSG(test, xfs_truncate(G393_FILE, up), 0,
			    "truncate to %lld failed", up);

	g393_all_zero(test, G393_FILE, up, "client", buf);
	g393_all_zero(test, G393_SERVER, up, "server", buf);
}

static void truncate_to_zero_then_up_reads_as_a_hole(struct kunit *test)
{
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G393_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g393_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G393_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* within what an inline-data filesystem would keep in the inode */
	g393_one_round(test, 50, buf);
	/* and a whole page, where the inline copy would have been dismissed */
	g393_one_round(test, 4096, buf);
}

static int g393_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g393_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g393_cases[] = {
	KUNIT_CASE(truncate_to_zero_then_up_reads_as_a_hole),
	{}
};

static struct kunit_suite g393_suite = {
	.name		= "xfstests/generic/393",
	.suite_init	= g393_suite_init,
	.suite_exit	= g393_suite_exit,
	.test_cases	= g393_cases,
};

kunit_test_suites(&g393_suite);

MODULE_DESCRIPTION("xfstests generic/393 over a loopback NFS mount");
MODULE_LICENSE("GPL");
