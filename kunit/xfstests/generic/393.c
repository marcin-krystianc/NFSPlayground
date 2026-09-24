// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/393 over a loopback NFS mount: small truncations of
 * inline data and its cached page.
 *
 * Upstream has four scenarios, each one xfs_io -t -f run on a fresh file:
 * write 40 bytes of 0x58, fsync, then truncate twice --
 *
 *	truncate 0, then 50	page #0 truncated entirely, re-extended within
 *				what an inline-data inode could hold
 *	truncate 0, then 4096	... re-extended to a whole page
 *	truncate 4, then 50	page #0 truncated partially
 *	truncate 4, then 4096	... re-extended to a whole page
 *
 * -- and dumps the file with od before and after cycling the mount. It
 * was written for ext4/f2fs inline data, where the bytes live inside the
 * inode and a truncate has to clear both the inode copy and the cached
 * page, but the property is general: whatever the first truncate cut off
 * reads back as zeroes after the second one extends the file.
 *
 * Over NFS the truncates are two SETATTRs carrying the open file's
 * stateid, and the client has to drop or trim the page it still holds
 * from the write (nfs_vmtruncate() -> truncate_pagecache()) rather than
 * serve the old 0x58s back from it.
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

/* the od dump: `keep` bytes of 0x58, then zeroes up to `size` */
static void g393_expect(struct kunit *test, const char *path, loff_t keep,
			loff_t size, const char *which, u8 *buf)
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
		if (buf[i] != (i < keep ? 0x58 : 0)) {
			KUNIT_FAIL(test, "%s: byte %lld is %02x, expected %02x",
				   which, i, buf[i], i < keep ? 0x58 : 0);
			return;
		}
}

/* xfs_io -t -f -c "pwrite -S 0x58 0 40" -c fsync -c "truncate down" -c "truncate up" */
static void g393_one_round(struct kunit *test, loff_t down, loff_t up,
			   u8 *buf)
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
	KUNIT_ASSERT_EQ_MSG(test, xfs_ftruncate(f, down), 0,
			    "truncate %lld failed", down);
	KUNIT_ASSERT_EQ_MSG(test, xfs_ftruncate(f, up), 0,
			    "truncate %lld failed", up);
	filp_close(f, NULL);

	g393_expect(test, G393_FILE, down, up, "client", buf);
	g393_expect(test, G393_SERVER, down, up, "server", buf);
}

static void truncate_down_then_up_reads_as_a_hole(struct kunit *test)
{
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G393_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g393_remove_tree, NULL),
			0);
	buf = kunit_kmalloc(test, G393_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* truncate inline_data after #0 page was truncated entirely */
	g393_one_round(test, 0, 50, buf);
	/* truncate dismissed inline_data after #0 page was truncated entirely */
	g393_one_round(test, 0, 4096, buf);
	/* truncate inline_data after #0 page was truncated partially */
	g393_one_round(test, 4, 50, buf);
	/* truncate dismissed inline_data after #0 page was truncated partially */
	g393_one_round(test, 4, 4096, buf);
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
	KUNIT_CASE(truncate_down_then_up_reads_as_a_hole),
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
