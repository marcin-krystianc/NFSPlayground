// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/255 over a loopback NFS mount: hole punching matrix.
 *
 * Upstream's punch-hole matrix: holes at aligned and unaligned offsets,
 * spanning page boundaries, and reaching exactly to EOF. POSIX semantics
 * say the entire requested range reads back as zeros regardless of
 * alignment (the server zero-fills partial blocks), neighbours are
 * untouched, and KEEP_SIZE holds the size. Every punch is an NFSv4.2
 * DEALLOCATE.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G255_ROOT	XFS_MNT "/g255"

#define G255_SZ	(128 * 1024)

static void g255_remove_tree(void *unused)
{
	xfs_unlink(G255_ROOT "/f");
	xfs_rmdir(G255_ROOT);
}

static const struct { loff_t off; loff_t len; } g255_holes[] = {
	{ 4096,		4096 },		/* page aligned */
	{ 20000,	5000 },		/* both edges unaligned */
	{ 32768,	3 * 4096 },	/* multi-page */
	{ 65536 + 1,	4094 },		/* interior of one page */
	{ G255_SZ - 8192, 8192 },	/* ends exactly at EOF */
};

static void every_punched_range_reads_zero(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	int h, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G255_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g255_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G255_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xEE, G255_SZ);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G255_ROOT "/f", buf, G255_SZ),
			0);

	f = filp_open(G255_ROOT "/f", O_RDWR, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (h = 0; h < ARRAY_SIZE(g255_holes); h++)
		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
						  FALLOC_FL_KEEP_SIZE,
						  g255_holes[h].off,
						  g255_holes[h].len),
				    0, "punch %d failed", h);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G255_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G255_SZ,
			    "KEEP_SIZE punching changed the size");

	/* build the expectation and verify the whole file in one pass */
	memset(buf, 0xEE, G255_SZ);
	for (h = 0; h < ARRAY_SIZE(g255_holes); h++)
		memset(buf + g255_holes[h].off, 0, g255_holes[h].len);
	{
		u8 *got = kunit_kmalloc(test, G255_SZ, GFP_KERNEL);
		ssize_t n;

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
		n = xfs_read_range(G255_ROOT "/f", got, G255_SZ, 0);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)G255_SZ);
		for (i = 0; i < G255_SZ; i++)
			if (got[i] != buf[i]) {
				KUNIT_FAIL(test,
					   "byte %d: %02x, expected %02x", i,
					   got[i], buf[i]);
				return;
			}
	}
}

static int g255_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g255_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g255_cases[] = {
	KUNIT_CASE(every_punched_range_reads_zero),
	{}
};

static struct kunit_suite g255_suite = {
	.name		= "xfstests/generic/255",
	.suite_init	= g255_suite_init,
	.suite_exit	= g255_suite_exit,
	.test_cases	= g255_cases,
};

kunit_test_suites(&g255_suite);

MODULE_DESCRIPTION("xfstests generic/255 over a loopback NFS mount");
MODULE_LICENSE("GPL");
