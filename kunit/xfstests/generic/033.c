// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/033 over a loopback NFS mount: ZERO_RANGE is refused.
 *
 * Upstream stresses XFS indirect block reservation for delayed allocation
 * extents: write 64K, then fzero every other 4K range to split one large
 * delalloc extent into many small ones, then fzero the opposite set to
 * remove the rest. On XFS the file ends up entirely zeroed, which is what
 * 033.out records.
 *
 * NFSv4.2 has no ZERO_RANGE. nfs42_fallocate() accepts mode 0 (ALLOCATE) and
 * PUNCH_HOLE|KEEP_SIZE (DEALLOCATE) and rejects everything else
 * (fs/nfs/nfs4file.c:228), so every one of upstream's sixteen fzero calls
 * returns EOPNOTSUPP and the file keeps its data. **The golden output
 * inverts**: upstream expects 64K of zeroes, this expects 64K of 0xcd. That
 * is not a porting shortcut, it is the correct answer for a filesystem that
 * does not implement the operation, and the same shape as the collapse
 * family (generic/021, 012, 016, 022, 031).
 *
 * This is a thin test and worth labelling as such: it pins one errno and one
 * no-op, and the delalloc-reservation bug it was written for has no NFS
 * analogue. Its value is that ZERO_RANGE is a mode a future client could
 * plausibly start accepting -- NFSv4.2 has no operation for it, so if this
 * ever stops returning EOPNOTSUPP something is emulating it client-side, and
 * the byte check says whether the emulation is correct.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G033_ROOT	XFS_MNT "/g033"
#define G033_FILE	G033_ROOT "/file"
#define G033_SERVER	XFS_EXPORT "/g033/file"

#define G033_BYTES	(64 * 1024)	/* upstream's $bytes */
#define G033_STEP	8192		/* upstream's stride */
#define G033_ZLEN	4096		/* upstream's fzero length */
#define G033_FILL	0xcd

static void g033_remove_tree(void *unused)
{
	xfs_unlink(G033_FILE);
	xfs_rmdir(G033_ROOT);
}

static void g033_verify_untouched(struct kunit *test, const char *path,
				  const char *which)
{
	struct kstat st;
	u8 *got;
	loff_t off;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G033_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G033_BYTES,
			    "%s: size is %lld, expected %d", which, st.size,
			    G033_BYTES);

	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	for (off = 0; off < G033_BYTES; off += PAGE_SIZE) {
		ssize_t r = xfs_read_range(path, got, PAGE_SIZE, off);
		size_t i;

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)PAGE_SIZE,
				    "%s: read at %lld returned %zd", which, off,
				    r);
		for (i = 0; i < PAGE_SIZE; i++)
			if (got[i] != G033_FILL) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x -- a refused ZERO_RANGE zeroed data",
					   which, off + (loff_t)i, got[i],
					   G033_FILL);
				return;
			}
	}
}

static void zero_range_is_refused_and_changes_nothing(struct kunit *test)
{
	struct file *f;
	u8 *buf;
	loff_t i, endoff;
	int err, calls = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G033_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g033_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, G033_FILL, PAGE_SIZE);

	f = filp_open(G033_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	for (i = 0; i < G033_BYTES; i += PAGE_SIZE) {
		loff_t pos = i;

		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, PAGE_SIZE, &pos),
				(ssize_t)PAGE_SIZE);
	}

	endoff = G033_BYTES - 4096;

	/* upstream's first pass: every other 4K range */
	for (i = 0; i <= endoff; i += G033_STEP) {
		err = vfs_fallocate(f, FALLOC_FL_ZERO_RANGE, i, G033_ZLEN);
		KUNIT_ASSERT_EQ_MSG(test, err, -EOPNOTSUPP,
				    "ZERO_RANGE at %lld returned %d, expected EOPNOTSUPP",
				    i, err);
		calls++;
	}

	/* upstream's second pass: the opposite set */
	for (i = 4096; i <= endoff; i += G033_STEP) {
		err = vfs_fallocate(f, FALLOC_FL_ZERO_RANGE, i, G033_ZLEN);
		KUNIT_ASSERT_EQ_MSG(test, err, -EOPNOTSUPP,
				    "ZERO_RANGE at %lld returned %d, expected EOPNOTSUPP",
				    i, err);
		calls++;
	}
	filp_close(f, NULL);

	/* both passes together cover upstream's whole 64K stride pattern */
	KUNIT_EXPECT_EQ_MSG(test, calls, 16,
			    "expected 16 ZERO_RANGE attempts, made %d", calls);

	g033_verify_untouched(test, G033_FILE, "client");
	g033_verify_untouched(test, G033_SERVER, "SERVER");
}

static int g033_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g033_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g033_cases[] = {
	KUNIT_CASE(zero_range_is_refused_and_changes_nothing),
	{}
};

static struct kunit_suite g033_suite = {
	.name		= "xfstests/generic/033",
	.suite_init	= g033_suite_init,
	.suite_exit	= g033_suite_exit,
	.test_cases	= g033_cases,
};

kunit_test_suites(&g033_suite);

MODULE_DESCRIPTION("xfstests generic/033 over a loopback NFS mount");
MODULE_LICENSE("GPL");
