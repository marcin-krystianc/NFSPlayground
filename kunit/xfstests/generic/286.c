// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/286 over a loopback NFS mount: sparse copy via SEEK.
 *
 * Upstream copies sparse files by walking SEEK_DATA/SEEK_HOLE and checks
 * the copy is faithful. Three data islands in 256K; the port walks the
 * source with SEEK, copies only data extents, and then proves (a) the
 * walk found exactly the three islands and (b) the copy is byte-identical
 * across the whole logical size, holes included.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G286_ROOT	XFS_MNT "/g286"

#define G286_SZ	(256 * 1024)

static const loff_t g286_islands[] = { 0, 100 * 1024, 200 * 1024 };
#define G286_ISLAND_SZ	8192

static void g286_remove_tree(void *unused)
{
	xfs_unlink(G286_ROOT "/src");
	xfs_unlink(G286_ROOT "/dst");
	xfs_rmdir(G286_ROOT);
}

static void seek_walk_copies_sparse_files_faithfully(struct kunit *test)
{
	struct file *in, *out;
	u8 *buf, *a, *b;
	loff_t pos = 0;
	int islands = 0, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G286_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g286_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G286_ISLAND_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	in = filp_open(G286_ROOT "/src", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	for (i = 0; i < ARRAY_SIZE(g286_islands); i++) {
		loff_t p = g286_islands[i];

		memset(buf, 0xB0 + i, G286_ISLAND_SZ);
		KUNIT_ASSERT_EQ(test,
				kernel_write(in, buf, G286_ISLAND_SZ, &p),
				(ssize_t)G286_ISLAND_SZ);
	}
	KUNIT_ASSERT_EQ(test, xfs_truncate(G286_ROOT "/src", G286_SZ), 0);
	KUNIT_ASSERT_EQ(test, vfs_fsync(in, 0), 0);

	out = filp_open(G286_ROOT "/dst", O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	/* the SEEK walk */
	for (;;) {
		loff_t data = vfs_llseek(in, pos, SEEK_DATA);
		loff_t hole;

		if (data == -ENXIO)
			break;
		KUNIT_ASSERT_GE(test, data, (loff_t)0);
		hole = vfs_llseek(in, data, SEEK_HOLE);
		KUNIT_ASSERT_GT(test, hole, data);
		islands++;

		while (data < hole) {
			size_t chunk = min_t(loff_t, hole - data,
					     G286_ISLAND_SZ);
			loff_t rp = data, wp = data;

			KUNIT_ASSERT_EQ(test,
					kernel_read(in, buf, chunk, &rp),
					(ssize_t)chunk);
			KUNIT_ASSERT_EQ(test,
					kernel_write(out, buf, chunk, &wp),
					(ssize_t)chunk);
			data += chunk;
		}
		pos = hole;
	}
	KUNIT_ASSERT_EQ(test, xfs_truncate(G286_ROOT "/dst", G286_SZ), 0);
	filp_close(in, NULL);
	filp_close(out, NULL);

	KUNIT_EXPECT_EQ_MSG(test, islands, (int)ARRAY_SIZE(g286_islands),
			    "the SEEK walk found %d data extents, expected %zu",
			    islands, ARRAY_SIZE(g286_islands));

	/* full byte-compare, holes included */
	a = kunit_kmalloc(test, G286_SZ, GFP_KERNEL);
	b = kunit_kmalloc(test, G286_SZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	KUNIT_ASSERT_EQ(test, xfs_read_range(G286_ROOT "/src", a, G286_SZ, 0),
			(ssize_t)G286_SZ);
	KUNIT_ASSERT_EQ(test, xfs_read_range(G286_ROOT "/dst", b, G286_SZ, 0),
			(ssize_t)G286_SZ);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(a, b, G286_SZ), 0,
			    "the seek-driven copy diverged from the source");
}

static int g286_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g286_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g286_cases[] = {
	KUNIT_CASE(seek_walk_copies_sparse_files_faithfully),
	{}
};

static struct kunit_suite g286_suite = {
	.name		= "xfstests/generic/286",
	.suite_init	= g286_suite_init,
	.suite_exit	= g286_suite_exit,
	.test_cases	= g286_cases,
};

kunit_test_suites(&g286_suite);

MODULE_DESCRIPTION("xfstests generic/286 over a loopback NFS mount");
MODULE_LICENSE("GPL");
