// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/406 over a loopback NFS mount: one direct write large
 * enough to be split.
 *
 * Upstream issues a single 258 MiB O_DIRECT pwrite, because btrfs split
 * any direct write of 128 MiB or more and then miscounted its outstanding
 * extents in the endio path. Its golden output is silence: the write must
 * simply complete.
 *
 * Over NFS every direct write is split, by wsize rather than by 128 MiB:
 * nfs_direct_write_schedule_iovec() walks the iterator handing out one
 * nfs_page per wsize-worth of pages, and each becomes its own WRITE RPC
 * whose completion has to be accounted before the request can be
 * considered done. So the interesting size here is "several times wsize",
 * not upstream's 258 MiB, which would not fit in the fixture's 64 MiB
 * tmpfs export anyway.
 *
 * The port writes 2 MiB in a single call and asserts that the mount's
 * negotiated wsize is at most half of it, so the premise -- that this
 * write really is split -- is checked rather than assumed. Every byte is
 * then compared both through the client and against the server's own
 * copy, which is a stronger ending than upstream's silence: a split that
 * loses or reorders a chunk shows up as wrong bytes.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>

#include "xfstests_nfs_fixture.h"

#define G406_ROOT	XFS_MNT "/g406"
#define G406_FILE	G406_ROOT "/testfile.406"
#define G406_SERVER	XFS_EXPORT "/g406/testfile.406"

#define G406_SIZE	(2 * 1024 * 1024)
#define G406_CHUNK	65536

/* every byte identifies its own offset, so a misplaced chunk is visible */
static u8 g406_byte(loff_t off)
{
	return (u8)(off >> 16) ^ (u8)off;
}

static void g406_remove_tree(void *unused)
{
	xfs_unlink(G406_FILE);
	xfs_rmdir_settled(G406_ROOT);
}

static void g406_verify(struct kunit *test, const char *path, u8 *buf,
			const char *which)
{
	loff_t off;
	int i;

	for (off = 0; off < G406_SIZE; off += G406_CHUNK) {
		ssize_t n = xfs_read_range(path, buf, G406_CHUNK, off);

		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G406_CHUNK,
				    "%s: read at %lld returned %zd", which,
				    off, n);
		for (i = 0; i < G406_CHUNK; i++)
			if (buf[i] != g406_byte(off + i)) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   which, off + i, buf[i],
					   g406_byte(off + i));
				return;
			}
	}
}

static void one_large_direct_write_lands_whole(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G406_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g406_remove_tree, NULL),
			0);

	/* one contiguous buffer: the whole point is a single write call */
	buf = kunit_kmalloc(test, G406_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G406_SIZE; i++)
		buf[i] = g406_byte(i);

	f = filp_open(G406_FILE, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	/* the premise: this write is several WRITE RPCs, not one */
	KUNIT_EXPECT_LE_MSG(test, NFS_SERVER(file_inode(f))->wsize,
			    (unsigned int)G406_SIZE / 2,
			    "wsize is %u, so a %d-byte write is not split",
			    NFS_SERVER(file_inode(f))->wsize, G406_SIZE);

	n = xfs_direct_write(f, buf, G406_SIZE, &pos);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G406_SIZE,
			    "the %d-byte direct write returned %zd",
			    G406_SIZE, n);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G406_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G406_SIZE,
			    "the file is %lld bytes", st.size);

	g406_verify(test, G406_FILE, buf, "client");
	g406_verify(test, G406_SERVER, buf, "server");
}

static int g406_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g406_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g406_cases[] = {
	KUNIT_CASE_SLOW(one_large_direct_write_lands_whole),
	{}
};

static struct kunit_suite g406_suite = {
	.name		= "xfstests/generic/406",
	.suite_init	= g406_suite_init,
	.suite_exit	= g406_suite_exit,
	.test_cases	= g406_cases,
};

kunit_test_suites(&g406_suite);

MODULE_DESCRIPTION("xfstests generic/406 over a loopback NFS mount");
MODULE_LICENSE("GPL");
