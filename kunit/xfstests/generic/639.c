// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/639 over a loopback NFS mount: writing next to data
 * the client no longer has cached.
 *
 * Upstream writes 32 bytes at offset 0, cycles the mount to empty the
 * page cache, writes another 32 bytes at offset 32, and requires all 64
 * bytes to be 0xcd. It came from a ceph bug in write_begin: the second
 * write lands in the same page as the first, and if the filesystem treats
 * that page as newly allocated instead of reading the first 32 bytes back
 * first, the original data is lost.
 *
 * That is exactly nfs_write_begin()'s problem too. A partial write into a
 * page the client does not hold has to either read the page from the
 * server first or record the dirty range precisely enough that the rest
 * of the page is never written back as zeroes.
 *
 * Deviations: the mount cycle is invalidate_inode_pages2() on the file's
 * mapping, which is the part of it that matters here -- the client keeps
 * nothing of the page. The result is checked on the client and again on
 * the server's own copy.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G639_ROOT	XFS_MNT "/g639"
#define G639_FILE	G639_ROOT "/test_write_begin"
#define G639_SERVER	XFS_EXPORT "/g639/test_write_begin"
#define G639_HALF	32

static void g639_remove_tree(void *unused)
{
	xfs_unlink(G639_FILE);
	xfs_rmdir_settled(G639_ROOT);
}

static void g639_verify(struct kunit *test, const char *path, const char *which)
{
	char buf[2 * G639_HALF];
	ssize_t n;
	int i;

	n = xfs_read_range(path, buf, sizeof(buf), 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)sizeof(buf),
			    "%s: read returned %zd", which, n);
	for (i = 0; i < sizeof(buf); i++)
		if (buf[i] != (char)0xcd) {
			KUNIT_FAIL(test, "%s: byte %d is %02x, expected cd",
				   which, i, (u8)buf[i]);
			return;
		}
}

static void a_write_beside_uncached_data_keeps_it(struct kunit *test)
{
	struct file *f;
	loff_t pos;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G639_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g639_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G639_HALF, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, G639_HALF);

	f = filp_open(G639_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	pos = 0;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G639_HALF, &pos),
			(ssize_t)G639_HALF);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	/* the mount cycle: the client keeps none of the page */
	KUNIT_ASSERT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);

	pos = G639_HALF;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G639_HALF, &pos),
			(ssize_t)G639_HALF);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	g639_verify(test, G639_FILE, "client");
	filp_close(f, NULL);
	g639_verify(test, G639_SERVER, "server");
}

static int g639_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g639_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g639_cases[] = {
	KUNIT_CASE(a_write_beside_uncached_data_keeps_it),
	{}
};

static struct kunit_suite g639_suite = {
	.name		= "xfstests/generic/639",
	.suite_init	= g639_suite_init,
	.suite_exit	= g639_suite_exit,
	.test_cases	= g639_cases,
};

kunit_test_suites(&g639_suite);

MODULE_DESCRIPTION("xfstests generic/639 over a loopback NFS mount");
MODULE_LICENSE("GPL");
