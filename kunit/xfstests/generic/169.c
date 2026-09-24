// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/169 over a loopback NFS mount: appended size survives a
 * cold re-read.
 *
 * Upstream is two files, each written and then checked twice -- once live,
 * by xfs_io's stat on the open file, and once after the filesystem has been
 * unmounted and mounted again, which is what makes it a test of the
 * recorded size rather than of the cached one. From its golden image:
 *
 *	xfs_io -a: three writes of 5k, fsync after each	-> stat.size = 15360
 *	remount						-> stat.size = 15360
 *	xfs_io -f: pwrite 0 5, fsync, pwrite 5 5	-> stat.size = 10
 *	remount						-> stat.size = 10
 *
 * The first file is appended to through one O_APPEND descriptor, whatever
 * offsets the pwrites name. The second is written at positions, and is the
 * interesting one: its final write is never fsynced, so the size has to be
 * right without one.
 *
 * A shared loopback fixture cannot cycle the client mount out from under the
 * other suites, so the cold read is taken from the server's own copy under
 * the tmpfs export -- the same substitution generic/029 and generic/030 use,
 * and a stronger one, since it is the server's bytes rather than a
 * re-populated client cache.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G169_ROOT	XFS_MNT "/g169"
#define G169_FILE	G169_ROOT "/testfile"
#define G169_NEXT	G169_ROOT "/nextfile"
#define G169_SRV_FILE	XFS_EXPORT "/g169/testfile"
#define G169_SRV_NEXT	XFS_EXPORT "/g169/nextfile"

static void g169_remove_tree(void *unused)
{
	xfs_unlink(G169_FILE);
	xfs_unlink(G169_NEXT);
	xfs_rmdir(G169_ROOT);
}

/* one O_APPEND write, optionally fsynced, as xfs_io -a would do it */
/* xfs_io's stat: fstat of the open file */
static void g169_expect_fsize(struct kunit *test, struct file *f, loff_t want,
			      const char *what)
{
	struct kstat st;

	KUNIT_ASSERT_EQ(test, vfs_getattr(&f->f_path, &st, STATX_BASIC_STATS,
					  AT_STATX_SYNC_AS_STAT), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, want,
			    "%s: stat.size = %lld, expected %lld", what,
			    st.size, want);
}

static void g169_expect_size(struct kunit *test, const char *path,
			     loff_t want, const char *what)
{
	struct kstat st;

	KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0, "%s: stat", what);
	KUNIT_EXPECT_EQ_MSG(test, st.size, want, "%s: stat.size = %lld, expected %lld",
			    what, st.size, want);
}

static void g169_pwrite(struct kunit *test, struct file *f, const void *buf,
			size_t len, loff_t off)
{
	KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, buf, len, &off), (ssize_t)len,
			    "pwrite %lld %zu", off, len);
}

static void appended_sizes_survive_a_cold_read(struct kunit *test)
{
	struct file *f;
	char rd[10];
	u8 *buf;
	int step;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G169_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g169_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 5120, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, 5120);

	/* "creating new file for io" */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G169_FILE, "", 0), 0);

	/* "appending 15k to new file, sync every 5k": xfs_io -a */
	f = filp_open(G169_FILE, O_RDWR | O_APPEND, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	for (step = 0; step < 3; step++) {
		g169_pwrite(test, f, buf, 5120, step * 5120LL);
		KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	}
	g169_expect_fsize(test, f, 15360, "stat after 15k");
	filp_close(f, NULL);

	/* "remounting scratch": the server's own file is 15360 bytes too */
	g169_expect_size(test, G169_SRV_FILE, 15360,
			 "server-side stat after 15k");
	g169_expect_size(test, G169_FILE, 15360, "stat after 15k, reopened");

	/* "appending 10 bytes to new file, sync at 5 bytes": xfs_io -f */
	f = filp_open(G169_NEXT, O_RDWR | O_CREAT, 0600);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	g169_pwrite(test, f, "abcde", 5, 0);
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	g169_pwrite(test, f, "fghij", 5, 5);
	g169_expect_fsize(test, f, 10, "stat after 10 bytes");
	filp_close(f, NULL);

	g169_expect_size(test, G169_SRV_NEXT, 10,
			 "server-side stat after 10 bytes");
	g169_expect_size(test, G169_NEXT, 10, "stat after 10 bytes, reopened");

	/* and the bytes themselves reached the server */
	KUNIT_ASSERT_EQ(test, xfs_read_range(G169_SRV_NEXT, rd, 10, 0),
			(ssize_t)10);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(rd, "abcdefghij", 10), 0,
			    "the writes did not reach the server");
}

static int g169_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g169_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g169_cases[] = {
	KUNIT_CASE(appended_sizes_survive_a_cold_read),
	{}
};

static struct kunit_suite g169_suite = {
	.name		= "xfstests/generic/169",
	.suite_init	= g169_suite_init,
	.suite_exit	= g169_suite_exit,
	.test_cases	= g169_cases,
};

kunit_test_suites(&g169_suite);

MODULE_DESCRIPTION("xfstests generic/169 over a loopback NFS mount");
MODULE_LICENSE("GPL");
