// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/430 over a loopback NFS mount: copy_file_range() into
 * new files.
 *
 * Upstream builds a 5000-byte file of five 1000-byte runs ('a' to 'e')
 * and then makes six copies of it: the whole file, its first 1000 bytes,
 * 3000 bytes from offset 1000, the last 1000 bytes, 2000 bytes from
 * offset 4000 (which runs past the end and so copies only 1000), and
 * 3000 bytes from offset 1000 written at offset 1000 of the destination,
 * which leaves a hole at its start. Each result is compared with the
 * corresponding part of the original.
 *
 * Over NFSv4.2 copy_file_range() is not a read plus a write: it is the
 * COPY operation, issued by nfs4_copy_file_range(), and the server does
 * the copying. So what the port checks is that the client gets the
 * offsets and the length onto the wire correctly, that a length running
 * past EOF is clamped rather than refused, and that a destination offset
 * leaves a real hole -- none of which involve the data passing through
 * the client at all.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G430_ROOT	XFS_MNT "/g430"
#define G430_FILE	G430_ROOT "/file"
#define G430_SIZE	5000
#define G430_RUN	1000

static const char * const g430_copies[] = {
	"copy", "beginning", "middle", "end", "beyond", "hole",
};

static void g430_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g430_copies); i++) {
		snprintf(path, sizeof(path), G430_ROOT "/%s", g430_copies[i]);
		xfs_unlink(path);
	}
	xfs_unlink(G430_FILE);
	xfs_rmdir_settled(G430_ROOT);
}

/* the byte the original file holds at off */
static u8 g430_byte(loff_t off)
{
	return 'a' + (u8)(off / G430_RUN);
}

/* copy len bytes from src_off of the original to dst_off of name */
static ssize_t g430_copy(struct kunit *test, const char *name, loff_t src_off,
			 loff_t dst_off, size_t len)
{
	struct file *src, *dst;
	char path[64];
	ssize_t n;

	snprintf(path, sizeof(path), G430_ROOT "/%s", name);
	src = filp_open(G430_FILE, O_RDONLY, 0);
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

static void g430_expect(struct kunit *test, const char *name, loff_t off,
			size_t len, loff_t src_off, u8 *buf)
{
	char path[64];
	ssize_t n;
	int i;

	snprintf(path, sizeof(path), G430_ROOT "/%s", name);
	n = xfs_read_range(path, buf, len, off);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
			    "%s: read at %lld returned %zd", name, off, n);
	for (i = 0; i < len; i++) {
		u8 want = src_off < 0 ? 0x00 : g430_byte(src_off + i);

		if (buf[i] != want) {
			KUNIT_FAIL(test, "%s: byte %lld is %02x, expected %02x",
				   name, off + i, buf[i], want);
			return;
		}
	}
}

static void copy_file_range_into_new_files(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G430_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g430_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G430_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G430_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	for (i = 0; i < G430_SIZE / G430_RUN; i++) {
		memset(buf, 'a' + i, G430_RUN);
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G430_RUN, &pos),
				(ssize_t)G430_RUN);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	/* the whole file */
	KUNIT_EXPECT_EQ_MSG(test, g430_copy(test, "copy", 0, 0, G430_SIZE),
			    (ssize_t)G430_SIZE, "copying the whole file");
	g430_expect(test, "copy", 0, G430_SIZE, 0, buf);

	/* its beginning */
	KUNIT_EXPECT_EQ(test, g430_copy(test, "beginning", 0, 0, G430_RUN),
			(ssize_t)G430_RUN);
	g430_expect(test, "beginning", 0, G430_RUN, 0, buf);

	/* its middle */
	KUNIT_EXPECT_EQ(test, g430_copy(test, "middle", 1000, 0, 3000),
			3000L);
	g430_expect(test, "middle", 0, 3000, 1000, buf);

	/* its end */
	KUNIT_EXPECT_EQ(test, g430_copy(test, "end", 4000, 0, 1000), 1000L);
	g430_expect(test, "end", 0, 1000, 4000, buf);

	/* past its end: only what exists is copied */
	KUNIT_EXPECT_EQ_MSG(test, g430_copy(test, "beyond", 4000, 0, 2000),
			    1000L,
			    "a copy running past EOF did not stop at it");
	g430_expect(test, "beyond", 0, 1000, 4000, buf);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G430_ROOT "/beyond", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 1000LL,
			    "the copy beyond EOF is %lld bytes", st.size);

	/* and one that leaves a hole at the start of the destination */
	KUNIT_EXPECT_EQ(test, g430_copy(test, "hole", 1000, 1000, 3000),
			3000L);
	g430_expect(test, "hole", 0, 1000, -1, buf);
	g430_expect(test, "hole", 1000, 3000, 1000, buf);
}

static int g430_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g430_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g430_cases[] = {
	KUNIT_CASE(copy_file_range_into_new_files),
	{}
};

static struct kunit_suite g430_suite = {
	.name		= "xfstests/generic/430",
	.suite_init	= g430_suite_init,
	.suite_exit	= g430_suite_exit,
	.test_cases	= g430_cases,
};

kunit_test_suites(&g430_suite);

MODULE_DESCRIPTION("xfstests generic/430 over a loopback NFS mount");
MODULE_LICENSE("GPL");
