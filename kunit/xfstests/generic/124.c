// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/124 over a loopback NFS mount: preallocate, write a
 * positional pattern, read it back.
 *
 * src/iopat.c (built as preallo_rw_pattern_writer and _reader) fills a
 * 1 MiB buffer with the 64-bit counters 0..131071. The writer opens the
 * file O_RDWR | O_CREAT, preallocates 1 MiB with XFS_IOC_RESVSP and
 * writes the buffer in one write(2); the reader, a second process, opens
 * it the same way, reads 1 MiB in one read(2) and fails on the first
 * value that is not its own index. Upstream runs writer, reader and rm
 * 100 times on the test filesystem and 100 times on the scratch one.
 *
 * The pattern is positional, so a byte that comes back from the wrong
 * offset is caught, not just a byte that comes back wrong -- which is what
 * makes it worth having over NFS, where a 1 MiB write is split into
 * several WRITE RPCs and reassembled on the server.
 *
 * Deviations: the xfsctl preallocation has no meaning outside XFS (it
 * silently fails upstream on every other filesystem); here it is the
 * NFSv4.2 ALLOCATE that stands in for it, and -EOPNOTSUPP is tolerated.
 * There is one filesystem, so the two passes run in two directories on
 * it. Writer and reader are two opens rather than two processes; the
 * last round of each pass also checks the server's copy.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G124_ROOT	XFS_MNT "/g124"
#define G124_VALUES	131072			/* 64-bit counters */
#define G124_SIZE	(G124_VALUES * sizeof(s64))	/* 1048576 */
#define G124_ROUNDS	100

static const char * const g124_dirs[] = { "test", "scratch" };

static void g124_remove_tree(void *unused)
{
	char path[96];
	int i;

	xfs_settle_fput();
	for (i = 0; i < ARRAY_SIZE(g124_dirs); i++) {
		snprintf(path, sizeof(path), G124_ROOT "/%s/rw_pattern.tmp",
			 g124_dirs[i]);
		xfs_unlink(path);
		snprintf(path, sizeof(path), G124_ROOT "/%s", g124_dirs[i]);
		xfs_rmdir_settled(path);
	}
	xfs_rmdir_settled(G124_ROOT);
}

static void g124_kvfree(void *p)
{
	kvfree(p);
}

/* preallo_rw_pattern_writer */
static void g124_writer(struct kunit *test, const char *path, s64 *x)
{
	struct file *f;
	loff_t pos = 0;
	long i;
	int err;

	f = filp_open(path, O_RDWR | O_CREAT, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open for write: %ld",
			       PTR_ERR(f));
	/* upstream's XFS_IOC_RESVSP, which over NFSv4.2 is ALLOCATE */
	err = vfs_fallocate(f, 0, 0, G124_SIZE);
	KUNIT_EXPECT_TRUE_MSG(test, err == 0 || err == -EOPNOTSUPP,
			      "preallocating %lu bytes returned %d",
			      (unsigned long)G124_SIZE, err);
	for (i = 0; i < G124_VALUES; i++)
		x[i] = i;
	KUNIT_EXPECT_EQ_MSG(test, kernel_write(f, x, G124_SIZE, &pos),
			    (ssize_t)G124_SIZE, "the 1 MiB write was short");
	filp_close(f, NULL);
}

/* preallo_rw_pattern_reader */
static void g124_reader(struct kunit *test, const char *path, s64 *x,
			int flags, const char *which)
{
	struct file *f;
	loff_t pos = 0;
	long i;

	for (i = 0; i < G124_VALUES; i++)
		x[i] = -1;
	f = filp_open(path, flags, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open for read: %ld",
			       which, PTR_ERR(f));
	KUNIT_EXPECT_EQ_MSG(test, kernel_read(f, x, G124_SIZE, &pos),
			    (ssize_t)G124_SIZE, "%s: the 1 MiB read was short",
			    which);
	filp_close(f, NULL);
	for (i = 0; i < G124_VALUES; i++)
		if (x[i] != i) {
			KUNIT_FAIL(test, "%s: error: %ld %ld %lld", which, i,
				   8 * i, x[i]);
			return;
		}
}

static void the_positional_pattern_survives_every_round(struct kunit *test)
{
	char path[96], server[96];
	s64 *x;
	int d, round;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G124_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g124_remove_tree, NULL),
			0);
	x = kvmalloc(G124_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, x);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, g124_kvfree, x),
			0);

	for (d = 0; d < ARRAY_SIZE(g124_dirs); d++) {
		snprintf(path, sizeof(path), G124_ROOT "/%s", g124_dirs[d]);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		snprintf(path, sizeof(path), G124_ROOT "/%s/rw_pattern.tmp",
			 g124_dirs[d]);
		snprintf(server, sizeof(server),
			 XFS_EXPORT "/g124/%s/rw_pattern.tmp", g124_dirs[d]);
		for (round = 0; round < G124_ROUNDS; round++) {
			g124_writer(test, path, x);
			g124_reader(test, path, x, O_RDWR | O_CREAT, "client");
			if (round == G124_ROUNDS - 1)
				g124_reader(test, server, x, O_RDONLY, "server");
			xfs_settle_fput();
			KUNIT_ASSERT_EQ(test, xfs_unlink(path), 0);
		}
	}
}

static int g124_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g124_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g124_cases[] = {
	KUNIT_CASE_SLOW(the_positional_pattern_survives_every_round),
	{}
};

static struct kunit_suite g124_suite = {
	.name		= "xfstests/generic/124",
	.suite_init	= g124_suite_init,
	.suite_exit	= g124_suite_exit,
	.test_cases	= g124_cases,
};

kunit_test_suites(&g124_suite);

MODULE_DESCRIPTION("xfstests generic/124 over a loopback NFS mount");
MODULE_LICENSE("GPL");
