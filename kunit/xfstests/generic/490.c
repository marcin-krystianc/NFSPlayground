// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/490 over a loopback NFS mount: SEEK_DATA from the
 * middle of a large hole.
 *
 * Upstream runs seek_sanity_test cases 19 and 20. Both write a single
 * block at the very end of a sparse file and then seek for data from
 * several offsets inside the hole; every seek must land on that last
 * block. The offsets are chosen to fall in each tier of ext2/3's
 * indirect-block tree -- direct, indirect, double and triple -- which is
 * where ext4 once reported the hole as shorter than it was.
 *
 * Over NFSv4.2 SEEK is an RPC and the answer comes from the server's own
 * extent knowledge, so what the port checks is that a large offset
 * survives the round trip as an unsigned 64-bit offset4 and that the
 * client returns the server's answer rather than a clamped or cached one.
 * The tiers are kept because they cost nothing: the file is sparse, so
 * even the 34 GiB one occupies a single block on the tmpfs export.
 *
 * Deviations: the block size is 4096 rather than probed. Case 20's file
 * is enormous; upstream tolerates EFBIG there and reports success, and so
 * does the port.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G490_ROOT	XFS_MNT "/g490"
#define G490_FILE19	G490_ROOT "/seek19"
#define G490_FILE20	G490_ROOT "/seek20"

#define G490_BSZ	4096LL

static void g490_remove_tree(void *unused)
{
	xfs_unlink(G490_FILE20);
	xfs_unlink(G490_FILE19);
	xfs_rmdir_settled(G490_ROOT);
}

/* write one block at filsz - bsz, leaving everything before it a hole */
static struct file *g490_sparse_file(struct kunit *test, const char *path,
				     loff_t filsz, u8 *buf, bool *too_big)
{
	struct file *f;
	loff_t pos = filsz - G490_BSZ;
	ssize_t n;

	*too_big = false;
	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", path,
			       PTR_ERR(f));

	n = kernel_write(f, buf, G490_BSZ, &pos);
	if (n == -EFBIG) {
		/* upstream: "fs doesn't support so large files", reported
		 * as success
		 */
		*too_big = true;
		return f;
	}
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G490_BSZ,
			    "%s: writing the last block returned %zd", path,
			    n);
	return f;
}

static void g490_seek_data(struct kunit *test, struct file *f, loff_t from,
			   loff_t want, const char *what)
{
	loff_t got = vfs_llseek(f, from, SEEK_DATA);

	KUNIT_EXPECT_EQ_MSG(test, got, want,
			    "SEEK_DATA from %lld (%s) returned %lld, expected %lld",
			    from, what, got, want);
}

static void seek_data_from_inside_a_large_hole(struct kunit *test)
{
	loff_t filsz, data;
	struct file *f;
	bool too_big;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G490_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g490_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G490_BSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'a', G490_BSZ);

	/* case 19: just beyond the ext[23] indirect tree */
	filsz = (12 + G490_BSZ / 4 + 1) * G490_BSZ;
	data = filsz - G490_BSZ;
	f = g490_sparse_file(test, G490_FILE19, filsz, buf, &too_big);
	KUNIT_ASSERT_FALSE(test, too_big);

	g490_seek_data(test, f, G490_BSZ, data, "one block in");
	g490_seek_data(test, f, 12 * G490_BSZ, data, "past the direct blocks");
	g490_seek_data(test, f, (12 + G490_BSZ / 4 - 8) * G490_BSZ, data,
		       "inside the indirect block");
	filp_close(f, NULL);
}

static void seek_data_from_inside_a_huge_hole(struct kunit *test)
{
	loff_t filsz, data;
	struct file *f;
	bool too_big;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G490_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g490_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G490_BSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'a', G490_BSZ);

	/* case 20: in the middle of the triple indirect tree */
	filsz = (12 + G490_BSZ / 4 + 8 * (G490_BSZ / 4) * (G490_BSZ / 4) +
		 2 * (G490_BSZ / 4) + 5) * G490_BSZ;
	data = filsz - G490_BSZ;
	f = g490_sparse_file(test, G490_FILE20, filsz, buf, &too_big);
	if (too_big) {
		kunit_skip(test, "the server refused a %lld byte file", filsz);
		return;
	}

	g490_seek_data(test, f, 14 * G490_BSZ, data, "indirect tier");
	g490_seek_data(test, f, (12 + 2 * (G490_BSZ / 4)) * G490_BSZ, data,
		       "double indirect tier");
	g490_seek_data(test, f,
		       (12 + G490_BSZ / 4 + (G490_BSZ / 4) * (G490_BSZ / 4) +
			3 * (G490_BSZ / 4) + 5) * G490_BSZ, data,
		       "triple indirect tier");
	g490_seek_data(test, f,
		       (12 + G490_BSZ / 4 + 7 * (G490_BSZ / 4) * (G490_BSZ / 4) +
			5 * (G490_BSZ / 4)) * G490_BSZ, data,
		       "triple indirect tier, later");
	g490_seek_data(test, f,
		       (12 + G490_BSZ / 4 + 8 * (G490_BSZ / 4) * (G490_BSZ / 4) +
			G490_BSZ / 4 + 11) * G490_BSZ, data,
		       "last tier");
	filp_close(f, NULL);
}

static int g490_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g490_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g490_cases[] = {
	KUNIT_CASE(seek_data_from_inside_a_large_hole),
	KUNIT_CASE(seek_data_from_inside_a_huge_hole),
	{}
};

static struct kunit_suite g490_suite = {
	.name		= "xfstests/generic/490",
	.suite_init	= g490_suite_init,
	.suite_exit	= g490_suite_exit,
	.test_cases	= g490_cases,
};

kunit_test_suites(&g490_suite);

MODULE_DESCRIPTION("xfstests generic/490 over a loopback NFS mount");
MODULE_LICENSE("GPL");
