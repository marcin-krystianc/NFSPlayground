// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/436 over a loopback NFS mount: SEEK_HOLE/SEEK_DATA over
 * preallocated space with dirty data in it.
 *
 * Upstream runs seek_sanity_test cases 13-16. Each preallocates 4 MiB,
 * writes one or two runs of 'a' into it without syncing, and checks where
 * SEEK_HOLE and SEEK_DATA land: preallocated but unwritten space must read
 * as a hole, the written runs as data. The cases skip themselves unless a
 * probe finds "unwritten extents": fallocate two allocation units, and
 * SEEK_DATA from 0 must not return 0.
 *
 * Over NFSv4.2 the preallocation is ALLOCATE and each seek is a SEEK RPC;
 * nfs42_proc_llseek() writes back the dirty range first, so the server
 * answers with the data in place. The tmpfs export passes the probe:
 * shmem_fallocate() leaves its folios !uptodate, and mapping_seek_hole_data()
 * reports those as a hole.
 *
 * The expectations follow seek_sanity_test's do_lseek(): SEEK_HOLE may also
 * return EOF, and -1 means ENXIO. xfstests runs the program with -f on
 * NFSv4.2, so the SEEK_DATA "default behaviour" allowance does not apply.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>

#include "xfstests_nfs_fixture.h"

#define G436_ROOT	XFS_MNT "/g436"
#define G436_BASE	G436_ROOT "/seek_sanity_testfile"
#define G436_FILSZ	(4 << 20)

static void g436_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_unlink(G436_BASE);
	for (i = 13; i <= 16; i++) {
		snprintf(path, sizeof(path), G436_BASE "%02d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G436_ROOT);
}

static void g436_pwrite(struct kunit *test, struct file *f, const void *buf,
			size_t len, loff_t pos)
{
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, len, &pos), (ssize_t)len);
}

/* seek_sanity_test.c:get_io_sizes(), without the XFS-geometry shortcut */
static loff_t g436_alloc_size(struct kunit *test, struct file *f)
{
	loff_t pos = 0, offset = 1, shift, alloc;
	struct kstat st;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G436_BASE, &st), 0);
	alloc = st.blksize;

	while (pos == 0 && offset < alloc) {
		offset <<= 1;
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
		g436_pwrite(test, f, "a", 1, offset);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE(test, pos, 0);
	}
	shift = offset >> 2;
	while (shift && offset < alloc) {
		KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
		g436_pwrite(test, f, "a", 1, offset);
		pos = vfs_llseek(f, 0, SEEK_DATA);
		KUNIT_ASSERT_GE(test, pos, 0);
		offset += pos ? -shift : shift;
		shift >>= 1;
	}
	if (!shift)
		offset += pos ? 0 : 1;
	return offset;
}

static void g436_lseek(struct kunit *test, int testnum, int subtest,
		       struct file *f, loff_t filsz, int origin, loff_t set,
		       loff_t exp)
{
	loff_t exp2 = exp, pos;

	if (origin == SEEK_HOLE && exp != -1)
		exp2 = filsz;
	pos = vfs_llseek(f, set, origin);
	if (exp == -1)
		KUNIT_EXPECT_EQ_MSG(test, pos, (loff_t)-ENXIO,
				    "%02d.%02d %s from %lld: expected ENXIO, got %lld",
				    testnum, subtest,
				    origin == SEEK_HOLE ? "SEEK_HOLE" : "SEEK_DATA",
				    set, pos);
	else
		KUNIT_EXPECT_TRUE_MSG(test, pos == exp || pos == exp2,
				      "%02d.%02d %s from %lld: expected %lld or %lld, got %lld",
				      testnum, subtest,
				      origin == SEEK_HOLE ? "SEEK_HOLE" : "SEEK_DATA",
				      set, exp, exp2, pos);
}

static struct file *g436_create(struct kunit *test, int testnum)
{
	char path[64];
	struct file *f;

	snprintf(path, sizeof(path), G436_BASE "%02d", testnum);
	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open %s: %ld", path,
			       PTR_ERR(f));
	return f;
}

static void seek_over_unwritten_extents_with_dirty_data(struct kunit *test)
{
	loff_t alloc, pos, bufsz, filsz;
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G436_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g436_remove_tree, NULL),
			0);

	/* test_basic_support(): the allocation unit and the probe */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G436_BASE, "", 0), 0);
	f = filp_open(G436_BASE, O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	alloc = g436_alloc_size(test, f);
	kunit_info(test, "Allocation size: %lld\n", alloc);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0), 0);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, alloc * 2), 0);
	pos = vfs_llseek(f, 0, SEEK_DATA);
	filp_close(f, NULL);
	if (pos == 0)
		kunit_skip(test, "File system does not support unwritten extents.");

	/* cases 13 and 14 write 14 pages at a time */
	bufsz = roundup(PAGE_SIZE * 14, alloc);
	buf = kunit_kmalloc(test, bufsz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'a', bufsz);
	filsz = G436_FILSZ;

	/* 13: HOLE - unwritten DATA in dirty page */
	f = g436_create(test, 13);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, filsz), 0);
	g436_pwrite(test, f, buf, bufsz, 0);
	g436_lseek(test, 13, 1, f, filsz, SEEK_HOLE, 0, bufsz);
	g436_lseek(test, 13, 2, f, filsz, SEEK_HOLE, 1, bufsz);
	g436_lseek(test, 13, 3, f, filsz, SEEK_DATA, 0, 0);
	g436_lseek(test, 13, 4, f, filsz, SEEK_DATA, 1, 1);
	filp_close(f, NULL);

	/* 14: two dirty runs with unwritten space between */
	f = g436_create(test, 14);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, filsz), 0);
	g436_pwrite(test, f, buf, bufsz, 0);
	g436_pwrite(test, f, buf, bufsz, 3 * bufsz);
	g436_lseek(test, 14, 1, f, filsz, SEEK_HOLE, 0, bufsz);
	g436_lseek(test, 14, 2, f, filsz, SEEK_HOLE, 1, bufsz);
	g436_lseek(test, 14, 3, f, filsz, SEEK_HOLE, 3 * bufsz, 4 * bufsz);
	g436_lseek(test, 14, 4, f, filsz, SEEK_DATA, 0, 0);
	g436_lseek(test, 14, 5, f, filsz, SEEK_DATA, 1, 1);
	g436_lseek(test, 14, 6, f, filsz, SEEK_DATA, bufsz, 3 * bufsz);
	filp_close(f, NULL);

	/* cases 15 and 16 write one allocation unit (at least a page) */
	bufsz = roundup(PAGE_SIZE, alloc);

	/* 15: one page written just after the end of the unwritten extent */
	f = g436_create(test, 15);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, filsz), 0);
	g436_pwrite(test, f, buf, bufsz, 0);
	g436_pwrite(test, f, buf, bufsz, filsz);
	g436_lseek(test, 15, 1, f, filsz + bufsz, SEEK_HOLE, 0, bufsz);
	g436_lseek(test, 15, 2, f, filsz + bufsz, SEEK_HOLE, 1, bufsz);
	g436_lseek(test, 15, 3, f, filsz + bufsz, SEEK_DATA, 0, 0);
	g436_lseek(test, 15, 4, f, filsz + bufsz, SEEK_DATA, 1, 1);
	g436_lseek(test, 15, 5, f, filsz + bufsz, SEEK_DATA, bufsz, filsz);
	filp_close(f, NULL);

	/* 16: one page in the middle of the unwritten extent */
	f = g436_create(test, 16);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, filsz), 0);
	g436_pwrite(test, f, buf, bufsz, 0);
	g436_pwrite(test, f, buf, bufsz, filsz / 2);
	g436_lseek(test, 16, 1, f, filsz, SEEK_HOLE, 0, bufsz);
	g436_lseek(test, 16, 2, f, filsz, SEEK_HOLE, 1, bufsz);
	g436_lseek(test, 16, 3, f, filsz, SEEK_DATA, 0, 0);
	g436_lseek(test, 16, 4, f, filsz, SEEK_DATA, 1, 1);
	g436_lseek(test, 16, 5, f, filsz, SEEK_DATA, bufsz, filsz / 2);
	g436_lseek(test, 16, 6, f, filsz, SEEK_HOLE, filsz / 2,
		   filsz / 2 + bufsz);
	filp_close(f, NULL);
}

static int g436_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g436_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g436_cases[] = {
	KUNIT_CASE(seek_over_unwritten_extents_with_dirty_data),
	{}
};

static struct kunit_suite g436_suite = {
	.name		= "xfstests/generic/436",
	.suite_init	= g436_suite_init,
	.suite_exit	= g436_suite_exit,
	.test_cases	= g436_cases,
};

kunit_test_suites(&g436_suite);

MODULE_DESCRIPTION("xfstests generic/436 over a loopback NFS mount");
MODULE_LICENSE("GPL");
