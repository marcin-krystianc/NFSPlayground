// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/432 over a loopback NFS mount: copy_file_range() over
 * an existing file, a thousand bytes at a time.
 *
 * generic/433's experiment at a larger scale: a 5000-byte file of five
 * 1000-byte runs is copied, and then the copy is rearranged with
 * copy_file_range()s from the original -- swap the first and last runs,
 * swap the two inner ones, and finally copy 4000 bytes from offset 1000
 * to offset 3000, which extends the copy to 7000 bytes.
 *
 * Over NFSv4.2 these are COPYs into an existing file at an offset, so
 * what the port checks is that each one overwrites exactly its own range
 * and that the last one extends the destination rather than being
 * clamped to its old size.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G432_ROOT	XFS_MNT "/g432"
#define G432_FILE	G432_ROOT "/file"
#define G432_COPY	G432_ROOT "/copy"
#define G432_RUN	1000
#define G432_SIZE	5000

static void g432_remove_tree(void *unused)
{
	xfs_unlink(G432_COPY);
	xfs_unlink(G432_FILE);
	xfs_rmdir_settled(G432_ROOT);
}

static ssize_t g432_copy(struct kunit *test, loff_t src_off, loff_t dst_off,
			 size_t len, bool create)
{
	struct file *src, *dst;
	ssize_t n;

	src = filp_open(G432_FILE, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "source open: %ld",
			       PTR_ERR(src));
	dst = filp_open(G432_COPY, O_RDWR | O_CREAT | (create ? O_TRUNC : 0),
			0644);
	if (IS_ERR(dst)) {
		filp_close(src, NULL);
		KUNIT_FAIL(test, "destination open: %ld", PTR_ERR(dst));
		return -1;
	}
	n = vfs_copy_file_range(src, src_off, dst, dst_off, len, 0);
	filp_close(dst, NULL);
	filp_close(src, NULL);
	return n;
}

/* the copy must be `runs` runs long and hold exactly those letters */
static void g432_expect(struct kunit *test, const char *runs, u8 *buf,
			const char *what)
{
	size_t nruns = strlen(runs);
	struct kstat st;
	int i, j;

	KUNIT_ASSERT_EQ(test, xfs_kstat(G432_COPY, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)(nruns * G432_RUN),
			    "%s: the copy is %lld bytes, expected %zu", what,
			    st.size, nruns * G432_RUN);

	for (i = 0; i < nruns; i++) {
		ssize_t n = xfs_read_range(G432_COPY, buf, G432_RUN,
					   (loff_t)i * G432_RUN);

		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G432_RUN,
				    "%s: read of run %d returned %zd", what, i,
				    n);
		for (j = 0; j < G432_RUN; j++)
			if (buf[j] != runs[i]) {
				KUNIT_FAIL(test,
					   "%s: run %d byte %d is %02x, expected '%c'",
					   what, i, j, buf[j], runs[i]);
				return;
			}
	}
}

static void thousand_byte_copies_rearrange_an_existing_file(struct kunit *test)
{
	struct file *f;
	loff_t pos = 0;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G432_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g432_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G432_RUN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G432_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	for (i = 0; i < G432_SIZE / G432_RUN; i++) {
		memset(buf, 'a' + i, G432_RUN);
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G432_RUN, &pos),
				(ssize_t)G432_RUN);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	KUNIT_EXPECT_EQ(test, g432_copy(test, 0, 0, G432_SIZE, true),
			(ssize_t)G432_SIZE);
	g432_expect(test, "abcde", buf, "the whole-file copy");

	/* swap the first and last runs */
	KUNIT_EXPECT_EQ(test, g432_copy(test, 4000, 0, 1000, false), 1000L);
	KUNIT_EXPECT_EQ(test, g432_copy(test, 0, 4000, 1000, false), 1000L);
	g432_expect(test, "ebcda", buf, "after swapping the ends");

	/* and the two inner ones */
	KUNIT_EXPECT_EQ(test, g432_copy(test, 1000, 3000, 1000, false),
			1000L);
	KUNIT_EXPECT_EQ(test, g432_copy(test, 3000, 1000, 1000, false),
			1000L);
	g432_expect(test, "edcba", buf, "after swapping the middle");

	/* 4000 bytes landing at 3000: the copy grows to 7000 */
	KUNIT_EXPECT_EQ(test, g432_copy(test, 1000, 3000, 4000, false),
			4000L);
	g432_expect(test, "edcbcde", buf, "after copying the tail");
}

static int g432_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g432_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g432_cases[] = {
	KUNIT_CASE(thousand_byte_copies_rearrange_an_existing_file),
	{}
};

static struct kunit_suite g432_suite = {
	.name		= "xfstests/generic/432",
	.suite_init	= g432_suite_init,
	.suite_exit	= g432_suite_exit,
	.test_cases	= g432_cases,
};

kunit_test_suites(&g432_suite);

MODULE_DESCRIPTION("xfstests generic/432 over a loopback NFS mount");
MODULE_LICENSE("GPL");
