// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/169 over a loopback NFS mount: appended size survives a
 * cold re-read.
 *
 * Upstream is two files, each written and then checked twice -- once live
 * and once after the filesystem has been unmounted and mounted again, which
 * is what makes it a test of the recorded size rather than of the cached
 * one. From its golden image:
 *
 *	three O_APPEND writes of 5k with fsync between   -> stat.size = 15360
 *	remount                                          -> stat.size = 15360
 *	two O_APPEND writes of 5 bytes, fsync after the first -> stat.size = 10
 *	remount                                          -> stat.size = 10
 *
 * The second file is the interesting one: its final write is never fsynced,
 * so the size has to be right without one.
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
static void g169_append(struct kunit *test, const char *path, const void *buf,
			size_t len, bool sync, const char *what)
{
	struct file *f;
	loff_t pos = 0;	/* O_APPEND ignores the offset */
	ssize_t n;

	f = filp_open(path, O_WRONLY | O_APPEND, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
			       PTR_ERR(f));
	n = kernel_write(f, buf, len, &pos);
	if (sync)
		KUNIT_EXPECT_EQ_MSG(test, vfs_fsync(f, 0), 0, "%s: fsync", what);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
			    "%s: wrote %zd of %zu bytes", what, n, len);
}

static void g169_expect_size(struct kunit *test, const char *path,
			     loff_t want, const char *what)
{
	struct kstat st;

	KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0, "%s: stat", what);
	KUNIT_EXPECT_EQ_MSG(test, st.size, want, "%s: stat.size = %lld, expected %lld",
			    what, st.size, want);
}

static void appended_sizes_survive_a_cold_read(struct kunit *test)
{
	u8 *buf;
	int step;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G169_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g169_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 5120, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x69, 5120);

	/* "appending 15k to new file, sync every 5k" */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G169_FILE, "", 0), 0);
	for (step = 1; step <= 3; step++) {
		g169_append(test, G169_FILE, buf, 5120, true, "5k append");
		g169_expect_size(test, G169_FILE, (loff_t)step * 5120,
				 "after a synced 5k append");
	}
	g169_expect_size(test, G169_FILE, 15360, "stat after 15k");

	/* the cold read: the server's own file is 15360 bytes too */
	g169_expect_size(test, G169_SRV_FILE, 15360,
			 "server-side stat after 15k");

	/*
	 * "appending 10 bytes to new file, sync at 5 bytes" -- the second
	 * write is deliberately not synced.
	 */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G169_NEXT, "", 0), 0);
	g169_append(test, G169_NEXT, "abcde", 5, true, "first 5 bytes");
	g169_append(test, G169_NEXT, "fghij", 5, false, "second 5 bytes");
	g169_expect_size(test, G169_NEXT, 10, "stat after 10 bytes");
	g169_expect_size(test, G169_SRV_NEXT, 10,
			 "server-side stat after 10 bytes");

	/* and the bytes themselves are in append order on the server */
	{
		char rd[10];

		KUNIT_ASSERT_EQ(test, xfs_read_range(G169_SRV_NEXT, rd, 10, 0),
				(ssize_t)10);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(rd, "abcdefghij", 10), 0,
				    "the appends did not reach the server in order");
	}
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
