// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/069 over a loopback NFS mount: O_APPEND writes.
 *
 * Upstream loops appending writes and checks the file grows exactly as
 * written. Over NFS, O_APPEND is client-implemented: the client must
 * serialise its own view of EOF (revalidating size before each append),
 * so 500 appends of varying sizes with the running total checked against
 * the server after every batch is a real exercise of size revalidation
 * plus WRITE. Content is stamped per-chunk and fully verified at the end.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G069_ROOT	XFS_MNT "/g069"

#define G069_APPENDS	500

static u32 g069_len(int i)
{
	/* deterministic, varied, 1..2048 */
	return (i * 2654435761u % 2048) + 1;
}

static void g069_remove_tree(void *unused)
{
	xfs_unlink(G069_ROOT "/f");
	xfs_rmdir(G069_ROOT);
}

static void appends_accumulate_exactly(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	loff_t expected = 0, pos;
	ssize_t n;
	int i;
	u32 j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G069_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g069_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G069_ROOT "/f", "", 0), 0);

	buf = kunit_kmalloc(test, 2048, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < G069_APPENDS; i++) {
		u32 len = g069_len(i);

		f = filp_open(G069_ROOT "/f", O_WRONLY | O_APPEND, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f));
		for (j = 0; j < len; j++)
			buf[j] = (u8)(i ^ j);
		pos = 0;	/* O_APPEND ignores the position */
		n = kernel_write(f, buf, len, &pos);
		filp_close(f, NULL);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
				    "append %d short: %zd", i, n);
		expected += len;

		if (i % 50 == 49) {
			KUNIT_ASSERT_EQ(test, xfs_kstat(G069_ROOT "/f", &st), 0);
			KUNIT_ASSERT_EQ_MSG(test, st.size, expected,
					    "after append %d: size %lld, expected %lld",
					    i, st.size, expected);
		}
	}

	KUNIT_ASSERT_EQ(test, xfs_kstat(G069_ROOT "/f", &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, expected);

	/* every appended chunk landed exactly once, in order */
	pos = 0;
	for (i = 0; i < G069_APPENDS; i++) {
		u32 len = g069_len(i);

		n = xfs_read_range(G069_ROOT "/f", buf, len, pos);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)len);
		for (j = 0; j < len; j++)
			if (buf[j] != (u8)(i ^ j)) {
				KUNIT_FAIL(test,
					   "chunk %d corrupt at byte %u", i, j);
				return;
			}
		pos += len;
	}
}

static int g069_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g069_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g069_cases[] = {
	KUNIT_CASE_SLOW(appends_accumulate_exactly),
	{}
};

static struct kunit_suite g069_suite = {
	.name		= "xfstests/generic/069",
	.suite_init	= g069_suite_init,
	.suite_exit	= g069_suite_exit,
	.test_cases	= g069_cases,
};

kunit_test_suites(&g069_suite);

MODULE_DESCRIPTION("xfstests generic/069 over a loopback NFS mount");
MODULE_LICENSE("GPL");
