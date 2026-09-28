// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/565 over a loopback NFS mount: copy_file_range from a
 * file on one mount to a file on another.
 *
 * Upstream writes 128K of 0x61 under TEST_DIR, runs "xfs_io -c
 * 'copy_range -l 128k'" into a new file under SCRATCH_MNT, and compares
 * the md5sums. A kernel that answers EXDEV makes the test notrun.
 *
 * Over NFS the two files are on different superblocks of the same server,
 * so vfs_copy_file_range() hands the copy to nfs4_copy_file_range(). Both
 * mounts reach the same server, so it sends a synchronous COPY
 * (nfs42_proc_copy()); it falls back to splice_copy_file_range() only on
 * EOPNOTSUPP or EXDEV.
 *
 * Deviations: TEST_DIR is XFS_MNT and SCRATCH_MNT is the fixture's second
 * mount of the same export (XFS_SCRATCH_MNT, nosharecache). Upstream's
 * TEST and SCRATCH are usually two exports; here the server sees one
 * filesystem. The md5sums are a byte compare, done on the server's copy
 * of the destination and on the destination read through its own mount.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G565_TESTDIR	XFS_MNT "/test-565"
#define G565_SRC	G565_TESTDIR "/file"
#define G565_DSTDIR	XFS_SCRATCH_MNT "/scratch-565"
#define G565_DST	G565_DSTDIR "/copy"
#define G565_SERVER	XFS_EXPORT "/scratch-565/copy"

#define G565_LEN	(128 * 1024)

static void g565_remove_tree(void *unused)
{
	xfs_unlink(G565_DST);
	xfs_rmdir_settled(G565_DSTDIR);
	xfs_unlink(G565_SRC);
	xfs_rmdir_settled(G565_TESTDIR);
	xfs_scratch_umount();
}

static void g565_verify(struct kunit *test, const char *path, u8 *buf,
			const char *which)
{
	int i;

	memset(buf, 0, G565_LEN);
	KUNIT_ASSERT_EQ_MSG(test, xfs_read_range(path, buf, G565_LEN, 0),
			    (ssize_t)G565_LEN, "%s: short read", which);
	for (i = 0; i < G565_LEN; i++)
		if (buf[i] != 0x61) {
			KUNIT_FAIL(test, "%s: byte %d is %02x, expected 61",
				   which, i, buf[i]);
			return;
		}
}

static void copy_range_across_mounts(struct kunit *test)
{
	struct file *in, *out;
	ssize_t n;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g565_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G565_TESTDIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G565_DSTDIR), 0);

	buf = kunit_kmalloc(test, G565_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x61, G565_LEN);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G565_SRC, buf, G565_LEN), 0);

	in = filp_open(G565_SRC, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(in), "open src: %ld", PTR_ERR(in));
	out = filp_open(G565_DST, O_WRONLY | O_CREAT, 0644);
	if (IS_ERR(out)) {
		filp_close(in, NULL);
		KUNIT_FAIL_AND_ABORT(test, "open dst: %ld", PTR_ERR(out));
	}
	KUNIT_EXPECT_PTR_NE(test, file_inode(in)->i_sb, file_inode(out)->i_sb);

	n = vfs_copy_file_range(in, 0, out, 0, G565_LEN, 0);
	filp_close(out, NULL);
	filp_close(in, NULL);
	if (n == -EXDEV)
		kunit_skip(test, "no cross-device copy_file_range");
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G565_LEN,
			    "copy_file_range returned %zd", n);

	g565_verify(test, G565_SERVER, buf, "SERVER");
	g565_verify(test, G565_DST, buf, "scratch mount");
}

static int g565_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g565_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g565_cases[] = {
	KUNIT_CASE(copy_range_across_mounts),
	{}
};

static struct kunit_suite g565_suite = {
	.name		= "xfstests/generic/565",
	.suite_init	= g565_suite_init,
	.suite_exit	= g565_suite_exit,
	.test_cases	= g565_cases,
};

kunit_test_suites(&g565_suite);

MODULE_DESCRIPTION("xfstests generic/565 over a loopback NFS mount");
MODULE_LICENSE("GPL");
