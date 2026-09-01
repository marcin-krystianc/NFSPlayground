// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/027 over a loopback NFS mount: ENOSPC across many directories.
 *
 * Upstream reserves 2M on a small filesystem, then runs eight processes
 * each filling its own directory with 1k files until ENOSPC, releases the
 * reservation, and loops a hundred times -- hunting for allocator and
 * ENOSPC-path bugs under concurrent pressure from many directories.
 *
 * Single-threaded port on the 16MB export: a 2MB reservation file, then
 * eight directories filled round-robin with 1k files until the server
 * says ENOSPC, then everything released and the cycle repeated. What it
 * checks that generic/204 (one directory, 8k files) does not: that
 * ENOSPC arrives cleanly no matter which directory the allocation is for,
 * that the reservation stays intact and readable across the whole squeeze
 * (a client that let a failing WRITE corrupt an unrelated file would show
 * up here), and that repeated cycles keep working -- freed space has to
 * come back round-robin too.
 *
 * Deviations: single-threaded, so the concurrency half of upstream's
 * intent is out of reach; 4 cycles rather than 100, and 8 directories as
 * upstream has 8 processes.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G_ROOT		XFS_MNT "/g027"

#define G_DIRS		8
#define G_CYCLES	4
#define G_RESERVE	(2 * 1024 * 1024)
#define G_MAXFILES	4096

static void g_remove_tree(void *unused)
{
	char buf[80];
	int d, i;

	for (d = 0; d < G_DIRS; d++) {
		for (i = 0; i < G_MAXFILES; i++) {
			snprintf(buf, sizeof(buf), G_ROOT "/d%d/f%d", d, i);
			if (xfs_unlink(buf) == -ENOENT)
				break;
		}
		snprintf(buf, sizeof(buf), G_ROOT "/d%d", d);
		xfs_rmdir(buf);
	}
	xfs_unlink(G_ROOT "/reserve");
	xfs_rmdir_settled(G_ROOT);
}

/* one 1k file; returns 0, -ENOSPC, or another errno */
static int g_one_file(const char *path, const u8 *buf)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	int err;

	f = filp_open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, buf, 1024, &pos);
	err = (n == 1024) ? vfs_fsync(f, 0) : ((n < 0) ? (int)n : -EIO);
	filp_close(f, NULL);
	if (err)
		xfs_unlink(path);
	return err;
}

static void enospc_arrives_cleanly_from_every_directory(struct kunit *test)
{
	char name[80];
	u8 *buf, *res, *back;
	int cycle, d, i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g_remove_tree, NULL), 0);

	buf = kunit_kmalloc(test, 1024, GFP_KERNEL);
	res = kunit_kmalloc(test, G_RESERVE, GFP_KERNEL);
	back = kunit_kmalloc(test, G_RESERVE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, res);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, back);
	memset(buf, 0x2B, 1024);
	for (i = 0; i < G_RESERVE; i++)
		res[i] = (u8)(i * 11 + (i >> 9));

	/* upstream's "Reserve 2M space" */
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G_ROOT "/reserve", res, G_RESERVE),
			0);

	for (d = 0; d < G_DIRS; d++) {
		snprintf(name, sizeof(name), G_ROOT "/d%d", d);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(name), 0);
	}

	for (cycle = 0; cycle < G_CYCLES; cycle++) {
		int made[G_DIRS] = { };
		int full = 0, total = 0;

		/* round-robin fill: every directory must hit ENOSPC cleanly */
		for (i = 0; i < G_MAXFILES && full < G_DIRS; i++) {
			for (d = 0; d < G_DIRS; d++) {
				if (made[d] < 0)
					continue;
				snprintf(name, sizeof(name),
					 G_ROOT "/d%d/f%d", d, made[d]);
				err = g_one_file(name, buf);
				if (err == -ENOSPC) {
					made[d] = -made[d] - 1;
					full++;
					continue;
				}
				KUNIT_ASSERT_EQ_MSG(test, err, 0,
						    "cycle %d dir %d file %d: %d",
						    cycle, d, made[d], err);
				made[d]++;
				total++;
			}
		}

		KUNIT_ASSERT_EQ_MSG(test, full, G_DIRS,
				    "cycle %d: only %d of %d directories reached ENOSPC after %d files",
				    cycle, full, G_DIRS, total);
		KUNIT_EXPECT_GT_MSG(test, total, 1000,
				    "cycle %d: only %d files fit", cycle, total);

		/*
		 * The reservation survived the squeeze, byte for byte -- read
		 * from the server's own copy under the export, not through
		 * the client. Reading through the client is satisfied from
		 * its page cache and proves nothing about what actually
		 * landed: established by mutation, where a client write path
		 * that drops a byte per folio left the client-side read
		 * passing and only the server-side read caught it.
		 */
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(XFS_EXPORT "/g027/reserve", back,
					       G_RESERVE, 0),
				(ssize_t)G_RESERVE);
		KUNIT_ASSERT_EQ_MSG(test, memcmp(res, back, G_RESERVE), 0,
				    "cycle %d: the reserved file was damaged by the ENOSPC squeeze",
				    cycle);

		/* release everything and wait for the space to come back */
		for (d = 0; d < G_DIRS; d++) {
			int n = (made[d] < 0) ? -made[d] - 1 : made[d];

			for (i = 0; i < n; i++) {
				snprintf(name, sizeof(name),
					 G_ROOT "/d%d/f%d", d, i);
				KUNIT_ASSERT_EQ(test, xfs_unlink(name), 0);
			}
		}
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_wait_for_free_bytes(8 * 1024 * 1024), 0,
				    "cycle %d: freed space never came back",
				    cycle);
	}
}

static int g_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=32768");
	return xfstests_nfs_get();
}

static void g_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g_cases[] = {
	KUNIT_CASE_SLOW(enospc_arrives_cleanly_from_every_directory),
	{}
};

static struct kunit_suite g_suite = {
	.name		= "xfstests/generic/027",
	.suite_init	= g_suite_init,
	.suite_exit	= g_suite_exit,
	.test_cases	= g_cases,
};

kunit_test_suites(&g_suite);

MODULE_DESCRIPTION("xfstests generic/027 over a loopback NFS mount");
MODULE_LICENSE("GPL");
