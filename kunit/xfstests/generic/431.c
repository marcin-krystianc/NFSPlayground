// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/431 over a loopback NFS mount: one-byte
 * copy_file_range()s.
 *
 * Upstream writes the five bytes "abcde" and then makes seven copies:
 * one byte from each offset in turn, one byte from offset 4 written at
 * offset 1 of the destination (so the result is a NUL and then 'e'), and
 * one byte from offset 5, which is at EOF and copies nothing.
 *
 * Over NFSv4.2 each of these is a COPY of one byte, which is the size
 * where an off-by-one in the offsets or the length is unmissable, and the
 * last one is the case where the client must return 0 rather than send a
 * COPY the server would refuse.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G431_ROOT	XFS_MNT "/g431"
#define G431_FILE	G431_ROOT "/file"

static const char g431_data[] = "abcde";

static const struct g431_case {
	const char	*name;
	loff_t		src_off;
	loff_t		dst_off;
	size_t		len;
	ssize_t		want;		/* bytes copy_file_range must report */
	const char	*expect;	/* the destination's whole content */
	size_t		expect_len;
} g431_cases_table[] = {
	{ "a", 0, 0, 1, 1, "a", 1 },
	{ "b", 1, 0, 1, 1, "b", 1 },
	{ "c", 2, 0, 1, 1, "c", 1 },
	{ "d", 3, 0, 1, 1, "d", 1 },
	{ "e", 4, 0, 1, 1, "e", 1 },
	{ "f", 4, 1, 1, 1, "\0e", 2 },
	{ "g", 5, 0, 1, 0, "", 0 },
};

static void g431_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g431_cases_table); i++) {
		snprintf(path, sizeof(path), G431_ROOT "/%s",
			 g431_cases_table[i].name);
		xfs_unlink(path);
	}
	xfs_unlink(G431_ROOT "/copy");
	xfs_unlink(G431_FILE);
	xfs_rmdir_settled(G431_ROOT);
}

static ssize_t g431_copy(struct kunit *test, const char *name, loff_t src_off,
			 loff_t dst_off, size_t len)
{
	struct file *src, *dst;
	char path[64];
	ssize_t n;

	snprintf(path, sizeof(path), G431_ROOT "/%s", name);
	src = filp_open(G431_FILE, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "%s: source open: %ld",
			       name, PTR_ERR(src));
	dst = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(dst)) {
		filp_close(src, NULL);
		KUNIT_FAIL(test, "%s: open: %ld", name, PTR_ERR(dst));
		return -1;
	}
	n = vfs_copy_file_range(src, src_off, dst, dst_off, len, 0);
	filp_close(dst, NULL);
	filp_close(src, NULL);
	return n;
}

static void one_byte_copies_land_exactly(struct kunit *test)
{
	struct kstat st;
	char path[64];
	char got[8];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G431_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g431_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G431_FILE, g431_data,
					   sizeof(g431_data) - 1), 0);

	/* the whole file first, as upstream does */
	KUNIT_EXPECT_EQ(test, g431_copy(test, "copy", 0, 0,
					sizeof(g431_data) - 1),
			(ssize_t)(sizeof(g431_data) - 1));
	KUNIT_ASSERT_EQ(test,
			xfs_read_range(G431_ROOT "/copy", got,
				       sizeof(g431_data) - 1, 0),
			(ssize_t)(sizeof(g431_data) - 1));
	KUNIT_EXPECT_EQ_MSG(test,
			    memcmp(got, g431_data, sizeof(g431_data) - 1), 0,
			    "the whole-file copy differs");

	for (i = 0; i < ARRAY_SIZE(g431_cases_table); i++) {
		const struct g431_case *c = &g431_cases_table[i];

		KUNIT_EXPECT_EQ_MSG(test,
				    g431_copy(test, c->name, c->src_off,
					      c->dst_off, c->len),
				    c->want,
				    "%s: copy_file_range did not report %zd",
				    c->name, c->want);

		snprintf(path, sizeof(path), G431_ROOT "/%s", c->name);
		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)c->expect_len,
				    "%s: the copy is %lld bytes, expected %zu",
				    c->name, st.size, c->expect_len);
		if (!c->expect_len)
			continue;
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(path, got, c->expect_len, 0),
				(ssize_t)c->expect_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    memcmp(got, c->expect, c->expect_len), 0,
				    "%s: the copy holds the wrong bytes",
				    c->name);
	}
}

static int g431_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g431_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g431_cases[] = {
	KUNIT_CASE(one_byte_copies_land_exactly),
	{}
};

static struct kunit_suite g431_suite = {
	.name		= "xfstests/generic/431",
	.suite_init	= g431_suite_init,
	.suite_exit	= g431_suite_exit,
	.test_cases	= g431_cases,
};

kunit_test_suites(&g431_suite);

MODULE_DESCRIPTION("xfstests generic/431 over a loopback NFS mount");
MODULE_LICENSE("GPL");
