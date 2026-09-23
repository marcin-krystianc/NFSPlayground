// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/124 over a loopback NFS mount: preallocate, write a
 * positional pattern, read it back.
 *
 * src/iopat.c (built as preallo_rw_pattern_writer and _reader) fills a
 * 1 MiB buffer with the 64-bit counters 0..131071, preallocates 1 MiB with
 * XFS_IOC_RESVSP, writes the buffer in one call, and then in a second
 * process reads it back and fails on the first value that is not its own
 * index. Upstream repeats that 100 times on the test filesystem and 100
 * times on the scratch one.
 *
 * The pattern is positional, so a byte that comes back from the wrong
 * offset is caught, not just a byte that comes back wrong -- which is what
 * makes it worth having over NFS, where a 1 MiB write is split into
 * several WRITE RPCs and reassembled on the server.
 *
 * Deviations: the xfsctl preallocation has no meaning outside XFS (it
 * silently fails upstream on every other filesystem); here it is the
 * NFSv4.2 ALLOCATE that stands in for it, and -EOPNOTSUPP is tolerated so
 * the port still runs against a server without ALLOCATE. The iteration
 * count is 20 rather than 100, and the write/read split is two file
 * descriptors rather than two processes. Each round reads back through a
 * freshly opened file, and the last one also checks the server's copy.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G124_ROOT	XFS_MNT "/g124"
#define G124_FILE	G124_ROOT "/rw_pattern.tmp"
#define G124_SERVER	XFS_EXPORT "/g124/rw_pattern.tmp"

#define G124_VALUES	131072			/* 64-bit counters */
#define G124_SIZE	(G124_VALUES * sizeof(s64))
#define G124_CHUNK	65536			/* 8192 values per chunk */
#define G124_PER_CHUNK	(G124_CHUNK / sizeof(s64))
#define G124_ROUNDS	20			/* upstream: 100 */

static void g124_remove_tree(void *unused)
{
	xfs_unlink(G124_FILE);
	xfs_rmdir_settled(G124_ROOT);
}

static void g124_fill(s64 *buf, long first)
{
	long i;

	for (i = 0; i < G124_PER_CHUNK; i++)
		buf[i] = first + i;
}

static void g124_write_pattern(struct kunit *test, s64 *buf)
{
	struct file *f;
	loff_t pos = 0;
	long done;
	int err;

	f = filp_open(G124_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open for write: %ld",
			       PTR_ERR(f));

	/* upstream's XFS_IOC_RESVSP, which over NFSv4.2 is ALLOCATE */
	err = vfs_fallocate(f, 0, 0, G124_SIZE);
	KUNIT_EXPECT_TRUE_MSG(test, err == 0 || err == -EOPNOTSUPP,
			      "preallocating %lu bytes returned %d",
			      (unsigned long)G124_SIZE, err);

	for (done = 0; done < G124_VALUES; done += G124_PER_CHUNK) {
		g124_fill(buf, done);
		KUNIT_ASSERT_EQ_MSG(test,
				    kernel_write(f, buf, G124_CHUNK, &pos),
				    (ssize_t)G124_CHUNK,
				    "write at value %ld failed", done);
	}
	filp_close(f, NULL);
}

static void g124_check_pattern(struct kunit *test, const char *path, s64 *buf,
			       const char *which)
{
	struct file *f;
	loff_t pos = 0;
	long done, i;

	f = filp_open(path, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open for read: %ld",
			       which, PTR_ERR(f));

	for (done = 0; done < G124_VALUES; done += G124_PER_CHUNK) {
		KUNIT_ASSERT_EQ_MSG(test,
				    kernel_read(f, buf, G124_CHUNK, &pos),
				    (ssize_t)G124_CHUNK,
				    "%s: read at value %ld failed", which,
				    done);
		for (i = 0; i < G124_PER_CHUNK; i++)
			if (buf[i] != done + i) {
				KUNIT_FAIL(test,
					   "%s: value %ld (offset %ld) is %lld",
					   which, done + i,
					   (done + i) * (long)sizeof(s64),
					   buf[i]);
				filp_close(f, NULL);
				return;
			}
	}
	filp_close(f, NULL);
}

static void the_positional_pattern_survives_every_round(struct kunit *test)
{
	s64 *buf;
	int round;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G124_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g124_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G124_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (round = 0; round < G124_ROUNDS; round++) {
		g124_write_pattern(test, buf);
		g124_check_pattern(test, G124_FILE, buf, "client");
		if (round == G124_ROUNDS - 1)
			g124_check_pattern(test, G124_SERVER, buf, "server");
		KUNIT_ASSERT_EQ(test, xfs_unlink(G124_FILE), 0);
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
