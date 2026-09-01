// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/008 over a loopback NFS mount: zeroed ranges.
 *
 * The original exercises fallocate FALLOC_FL_ZERO_RANGE and checks the
 * tossed ranges read back as zeros. On NFS, xfstests _notruns at
 * `_require_xfs_io_command "fzero"`: the client's nfs42_fallocate()
 * accepts only mode 0 (ALLOCATE) and PUNCH_HOLE|KEEP_SIZE (DEALLOCATE) --
 * fs/nfs/nfs4file.c returns EOPNOTSUPP for everything else, ZERO_RANGE and
 * COLLAPSE_RANGE included (the latter is why generic/012 is not ported).
 *
 * The port pins that contract, then tests the same property -- ranges that
 * must read back as zeros -- through the operations NFSv4.2 does have:
 *
 *   - DEALLOCATE: punch a hole in the middle of known data; the hole reads
 *     zero, both neighbours are untouched, the size is unchanged
 *   - ALLOCATE: preallocate past EOF; the size grows and the new tail
 *     reads zero
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G008_ROOT	XFS_MNT "/g008"
#define G008_FILE	G008_ROOT "/ranges"
#define G008_SZ		(256 * 1024)
#define G008_HOLE_OFF	(64 * 1024)
#define G008_HOLE_LEN	(32 * 1024)
#define G008_ALLOC_LEN	(64 * 1024)

static void g008_remove_tree(void *unused)
{
	xfs_unlink(G008_FILE);
	xfs_rmdir(G008_ROOT);
}

static void g008_expect_range(struct kunit *test, loff_t off, size_t len,
			      u8 expected, const char *what)
{
	u8 *buf;
	size_t i;
	ssize_t got;

	buf = kunit_kmalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	got = xfs_read_range(G008_FILE, buf, len, off);
	KUNIT_ASSERT_EQ_MSG(test, got, (ssize_t)len,
			    "%s: short read at %lld", what, off);
	for (i = 0; i < len; i++)
		if (buf[i] != expected) {
			KUNIT_FAIL(test,
				   "%s: byte %lld is %02x, expected %02x",
				   what, off + (loff_t)i, buf[i], expected);
			return;
		}
}

static void zero_range_is_rejected_but_punch_and_alloc_zero_ranges(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *data;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G008_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g008_remove_tree,
						  NULL), 0);

	data = kunit_kmalloc(test, G008_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, data);
	memset(data, 0xAB, G008_SZ);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G008_FILE, data, G008_SZ), 0);

	f = filp_open(G008_FILE, O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* the notrun mirror: ZERO_RANGE has no NFSv4.2 mapping */
	err = vfs_fallocate(f, FALLOC_FL_ZERO_RANGE, G008_HOLE_OFF,
			    G008_HOLE_LEN);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "ZERO_RANGE over NFS: expected EOPNOTSUPP, got %d",
			    err);

	/* DEALLOCATE: hole reads zero, neighbours and size untouched */
	err = vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			    G008_HOLE_OFF, G008_HOLE_LEN);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "DEALLOCATE (punch hole): %d", err);

	g008_expect_range(test, G008_HOLE_OFF, G008_HOLE_LEN, 0x00,
			  "punched hole");
	g008_expect_range(test, G008_HOLE_OFF - 4096, 4096, 0xAB,
			  "data before the hole");
	g008_expect_range(test, G008_HOLE_OFF + G008_HOLE_LEN, 4096, 0xAB,
			  "data after the hole");
	KUNIT_ASSERT_EQ(test, xfs_kstat(G008_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G008_SZ,
			    "punching with KEEP_SIZE changed the file size");

	/* ALLOCATE: mode 0 past EOF grows the file with zeros */
	err = vfs_fallocate(f, 0, G008_SZ, G008_ALLOC_LEN);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "ALLOCATE past EOF: %d", err);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G008_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)(G008_SZ + G008_ALLOC_LEN),
			    "ALLOCATE did not extend the file");
	g008_expect_range(test, G008_SZ, G008_ALLOC_LEN, 0x00,
			  "allocated tail");
}

static int g008_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g008_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g008_cases[] = {
	KUNIT_CASE(zero_range_is_rejected_but_punch_and_alloc_zero_ranges),
	{}
};

static struct kunit_suite g008_suite = {
	.name		= "xfstests/generic/008",
	.suite_init	= g008_suite_init,
	.suite_exit	= g008_suite_exit,
	.test_cases	= g008_cases,
};

kunit_test_suites(&g008_suite);

MODULE_DESCRIPTION("xfstests generic/008 over a loopback NFS mount");
MODULE_LICENSE("GPL");
