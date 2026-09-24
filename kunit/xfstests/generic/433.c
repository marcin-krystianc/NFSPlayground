// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/433 over a loopback NFS mount: copy_file_range() over
 * an existing file, one byte at a time.
 *
 * Upstream copies "abcde" to a second file and then rearranges the copy
 * with one-byte copies from the original: swap the first and last bytes
 * ("ebcda"), swap the two inner ones ("edcba"), and finally copy four
 * bytes from offset 1 to offset 3, which extends the copy to seven bytes
 * ("edcbcde").
 *
 * Over NFSv4.2 each step is a COPY into an existing file at a given
 * offset, so this is the case where the destination is not empty and the
 * copy must overwrite exactly its range and nothing else -- and the last
 * one, where the copy runs past the destination's end and has to extend
 * it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G433_ROOT	XFS_MNT "/g433"
#define G433_FILE	G433_ROOT "/file"
#define G433_COPY	G433_ROOT "/copy"

static void g433_remove_tree(void *unused)
{
	xfs_unlink(G433_COPY);
	xfs_unlink(G433_FILE);
	xfs_rmdir_settled(G433_ROOT);
}

/* copy len bytes from the original at src_off into the copy at dst_off */
static ssize_t g433_copy(struct kunit *test, loff_t src_off, loff_t dst_off,
			 size_t len, bool create)
{
	struct file *src, *dst;
	ssize_t n;

	src = filp_open(G433_FILE, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "source open: %ld",
			       PTR_ERR(src));
	dst = filp_open(G433_COPY, O_RDWR | O_CREAT | (create ? O_TRUNC : 0),
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

static void g433_expect(struct kunit *test, const char *want, const char *what)
{
	size_t len = strlen(want);
	struct kstat st;
	char got[16];

	KUNIT_ASSERT_EQ(test, xfs_kstat(G433_COPY, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)len,
			    "%s: the copy is %lld bytes, expected %zu", what,
			    st.size, len);
	KUNIT_ASSERT_EQ(test, xfs_read_range(G433_COPY, got, len, 0),
			(ssize_t)len);
	got[len] = '\0';
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, len), 0,
			    "%s: the copy holds \"%s\", expected \"%s\"", what,
			    got, want);
}

static void copies_rearrange_an_existing_file(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G433_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g433_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G433_FILE, "abcde", 5), 0);

	KUNIT_EXPECT_EQ(test, g433_copy(test, 0, 0, 5, true), 5L);
	g433_expect(test, "abcde", "the whole-file copy");

	/* swap the first and last bytes */
	KUNIT_EXPECT_EQ(test, g433_copy(test, 0, 4, 1, false), 1L);
	KUNIT_EXPECT_EQ(test, g433_copy(test, 4, 0, 1, false), 1L);
	g433_expect(test, "ebcda", "after swapping the ends");

	/* and the two inner ones */
	KUNIT_EXPECT_EQ(test, g433_copy(test, 1, 3, 1, false), 1L);
	KUNIT_EXPECT_EQ(test, g433_copy(test, 3, 1, 1, false), 1L);
	g433_expect(test, "edcba", "after swapping the middle");

	/* four bytes from offset 1, landing at 3: the copy grows to seven */
	KUNIT_EXPECT_EQ(test, g433_copy(test, 1, 3, 4, false), 4L);
	g433_expect(test, "edcbcde", "after copying the tail");
}

static int g433_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g433_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g433_cases[] = {
	KUNIT_CASE(copies_rearrange_an_existing_file),
	{}
};

static struct kunit_suite g433_suite = {
	.name		= "xfstests/generic/433",
	.suite_init	= g433_suite_init,
	.suite_exit	= g433_suite_exit,
	.test_cases	= g433_cases,
};

kunit_test_suites(&g433_suite);

MODULE_DESCRIPTION("xfstests generic/433 over a loopback NFS mount");
MODULE_LICENSE("GPL");
