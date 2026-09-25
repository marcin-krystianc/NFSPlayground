// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/539 over a loopback NFS mount: SEEK_HOLE finds a hole
 * made by PUNCH_HOLE.
 *
 * Upstream runs seek_sanity_test's test 21 alone: write three allocation
 * units of 'a', punch the middle one, then SEEK_DATA from 0 must return
 * 0, SEEK_HOLE from 0 must return the start of the punched unit, and
 * SEEK_DATA from there must return the start of the third unit.
 *
 * Over NFSv4.2 the punch is a DEALLOCATE RPC and each llseek is a SEEK
 * RPC (nfs42_proc_llseek()), answered from the tmpfs export's own
 * extent map. xfstests runs seek_sanity_test with -f on NFSv4.2
 * (_fstyp_has_non_default_seek_data_hole), so the test is not skipped
 * there as it would be on a filesystem with only the default behaviour.
 *
 * The allocation unit is found as seek_sanity_test's get_io_sizes() finds
 * it: start from st_blksize, then locate the smallest offset at which a
 * one-byte write is no longer reported as data at offset 0.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/slab.h>

#include "xfstests_nfs_fixture.h"

#define G539_ROOT	XFS_MNT "/g539"
#define G539_FILE	G539_ROOT "/seek_sanity_testfile.539"

static void g539_remove_tree(void *unused)
{
	xfs_unlink(G539_FILE);
	xfs_rmdir_settled(G539_ROOT);
}

/* seek_sanity_test.c:get_io_sizes(), without the XFS-geometry shortcut */
static loff_t g539_alloc_size(struct kunit *test, struct file *f)
{
	loff_t pos = 0, offset = 1, shift, alloc;
	struct kstat st;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G539_FILE, &st), 0);
	alloc = st.blksize;

	while (pos == 0 && offset < alloc) {
		loff_t p;

		offset <<= 1;
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
		p = offset;
		KUNIT_ASSERT_EQ(test, kernel_write(f, "a", 1, &p), (ssize_t)1);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE_MSG(test, pos, 0, "SEEK_DATA probe: %lld", pos);
	}

	shift = offset >> 2;
	while (shift && offset < alloc) {
		loff_t p = offset;

		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
		KUNIT_ASSERT_EQ(test, kernel_write(f, "a", 1, &p), (ssize_t)1);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE_MSG(test, pos, 0, "SEEK_DATA bisect: %lld", pos);
		offset += pos ? -shift : shift;
		shift >>= 1;
	}
	if (!shift)
		offset += pos ? 0 : 1;
	return offset;
}

static void seek_hole_finds_a_punched_hole(struct kunit *test)
{
	loff_t alloc, pos = 0;
	struct file *f;
	ssize_t n;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G539_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g539_remove_tree, NULL),
			0);

	f = filp_open(G539_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	alloc = g539_alloc_size(test, f);
	kunit_info(test, "Allocation size: %lld\n", alloc);
	KUNIT_ASSERT_GT(test, alloc, 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);

	/* alloc comes from st_blksize, which over NFS can be a whole wsize */
	buf = kvmalloc(alloc * 3, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'a', alloc * 3);
	n = kernel_write(f, buf, alloc * 3, &pos);
	kvfree(buf);
	KUNIT_ASSERT_EQ(test, n, (ssize_t)(alloc * 3));

	KUNIT_ASSERT_EQ(test,
			vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
				      FALLOC_FL_KEEP_SIZE, alloc, alloc), 0);

	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 0, SEEK_DATA), (loff_t)0);
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, 0, SEEK_HOLE), alloc);
	KUNIT_EXPECT_EQ(test, vfs_llseek(f, alloc, SEEK_DATA), alloc * 2);

	filp_close(f, NULL);
}

static int g539_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g539_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g539_cases[] = {
	KUNIT_CASE(seek_hole_finds_a_punched_hole),
	{}
};

static struct kunit_suite g539_suite = {
	.name		= "xfstests/generic/539",
	.suite_init	= g539_suite_init,
	.suite_exit	= g539_suite_exit,
	.test_cases	= g539_cases,
};

kunit_test_suites(&g539_suite);

MODULE_DESCRIPTION("xfstests generic/539 over a loopback NFS mount");
MODULE_LICENSE("GPL");
