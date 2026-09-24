// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/214 over a loopback NFS mount: writes into
 * preallocated ranges.
 *
 * Upstream's seven files, each one an "unwritten extent sanity check":
 *
 *	falloc 4096, read it		-- all zeroes
 *	falloc 512, write byte 0	-- one byte, the rest still zeroes
 *	falloc 512, write byte 256	-- same, in the middle
 *	falloc 512, write byte 511	-- same, at the end
 *	falloc 0x65c00, write 0x10000 at 0x12000, fsync, truncate to 0x16000,
 *					   then read it back with O_DIRECT
 *	write 16k then falloc the same range
 *	a scatter of writes around one falloc
 *
 * The property is always the same: preallocated space that has not been
 * written reads as zeroes, and writing part of it must not disturb the
 * rest.
 *
 * Over NFSv4.2 falloc is ALLOCATE, which on this fixture's tmpfs export
 * really does allocate zeroed space, so "unwritten extent" becomes "space
 * the server has but has never been written". The fifth case is the
 * interesting one over NFS: a buffered write, an fsync, a truncate that
 * lands inside the written range, and then a direct read, which bypasses
 * whatever the client still has cached.
 *
 * Deviations: none in the operations; the golden hexdumps become
 * byte-exact expectations.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G214_ROOT	XFS_MNT "/g214"
#define G214_MAX	(128 * 1024)

static void g214_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 1; i <= 7; i++) {
		snprintf(path, sizeof(path), G214_ROOT "/test214-%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G214_ROOT);
}

static int g214_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G214_ROOT), 0);
	return kunit_add_action_or_reset(test, g214_remove_tree, NULL);
}

static void g214_expect(struct kunit *test, const char *path, loff_t off,
			size_t len, u8 val, u8 *buf, const char *what)
{
	ssize_t n = xfs_read_range(path, buf, len, off);
	int i;

	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
			    "%s: read at %lld returned %zd", what, off, n);
	for (i = 0; i < len; i++)
		if (buf[i] != val) {
			KUNIT_FAIL(test, "%s: byte %lld is %02x, expected %02x",
				   what, off + i, buf[i], val);
			return;
		}
}

static void allocated_space_reads_as_zeroes(struct kunit *test)
{
	const char *path = G214_ROOT "/test214-1";
	struct file *f;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g214_setup(test), 0);
	buf = kunit_kmalloc(test, G214_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, 4096), 0);
	filp_close(f, NULL);

	g214_expect(test, path, 0, 4096, 0x00, buf, "the allocated range");
}

/* falloc 512, write one byte at off, and check the rest is untouched */
static void g214_one_byte(struct kunit *test, const char *path, loff_t off,
			  u8 *buf)
{
	struct file *f;
	loff_t pos = off;
	u8 byte = 0x41;

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, 512), 0);
	KUNIT_ASSERT_EQ(test, kernel_write(f, &byte, 1, &pos), 1L);
	filp_close(f, NULL);

	if (off)
		g214_expect(test, path, 0, off, 0x00, buf, "before the byte");
	g214_expect(test, path, off, 1, byte, buf, "the byte");
	if (off + 1 < 512)
		g214_expect(test, path, off + 1, 512 - off - 1, 0x00, buf,
			    "after the byte");
}

static void a_write_into_allocated_space_leaves_the_rest_alone(
		struct kunit *test)
{
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g214_setup(test), 0);
	buf = kunit_kmalloc(test, G214_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	g214_one_byte(test, G214_ROOT "/test214-2", 0, buf);
	g214_one_byte(test, G214_ROOT "/test214-3", 256, buf);
	g214_one_byte(test, G214_ROOT "/test214-4", 511, buf);
}

static void a_truncate_after_allocate_and_write(struct kunit *test)
{
	const char *path = G214_ROOT "/test214-5";
	struct file *f;
	loff_t pos = 0x12000;
	u8 *buf;
	int i;

	KUNIT_ASSERT_EQ(test, g214_setup(test), 0);
	buf = kunit_kmalloc(test, G214_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 0, 0x65c00), 0);
	memset(buf, 0xaa, 0x10000);
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 0x10000, &pos),
			(ssize_t)0x10000);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, 0x16000), 0);
	filp_close(f, NULL);

	/* upstream's direct read of what is really there */
	f = filp_open(path, O_RDONLY | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct open: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_ASSERT_EQ(test, xfs_direct_read(f, buf, 0x16000, &pos),
			(ssize_t)0x16000);
	filp_close(f, NULL);

	for (i = 0; i < 0x16000; i++) {
		u8 want = (i >= 0x12000) ? 0xaa : 0x00;

		if (buf[i] != want) {
			KUNIT_FAIL(test, "byte %x is %02x, expected %02x", i,
				   buf[i], want);
			return;
		}
	}
}

static void allocating_over_a_written_range(struct kunit *test)
{
	const char *path = G214_ROOT "/test214-6";
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g214_setup(test), 0);
	buf = kunit_kmalloc(test, G214_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x36, 16384);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 16384, &pos),
			(ssize_t)16384);
	KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, 0, 16384), 0,
			    "allocating over the written range failed");
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 16384LL, "the file is %lld bytes",
			    st.size);
	g214_expect(test, path, 0, 16384, 0x36, buf,
		    "the data the allocate covered");
}

static void a_scatter_of_writes_around_an_allocate(struct kunit *test)
{
	const char *path = G214_ROOT "/test214-7";
	struct file *f;
	loff_t pos;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, g214_setup(test), 0);
	buf = kunit_kmalloc(test, G214_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x37, G214_MAX);

	f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	pos = 551917;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 41182, &pos),
			(ssize_t)41182);
	KUNIT_ASSERT_EQ(test, vfs_fallocate(f, 0, 917633, 392230), 0);
	pos = 285771;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 77718, &pos),
			(ssize_t)77718);
	pos = 1136718;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 104115, &pos),
			(ssize_t)104115);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	/* each written range is intact, and the gaps are still holes */
	g214_expect(test, path, 285771, 77718, 0x37, buf, "the second write");
	g214_expect(test, path, 551917, 41182, 0x37, buf, "the first write");
	g214_expect(test, path, 1136718, 104115, 0x37, buf, "the third write");
	g214_expect(test, path, 363489, 1000, 0x00, buf, "a gap");
	g214_expect(test, path, 917633, 1000, 0x00, buf,
		    "the allocated range");
}

static int g214_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g214_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g214_cases[] = {
	KUNIT_CASE(allocated_space_reads_as_zeroes),
	KUNIT_CASE(a_write_into_allocated_space_leaves_the_rest_alone),
	KUNIT_CASE(a_truncate_after_allocate_and_write),
	KUNIT_CASE(allocating_over_a_written_range),
	KUNIT_CASE(a_scatter_of_writes_around_an_allocate),
	{}
};

static struct kunit_suite g214_suite = {
	.name		= "xfstests/generic/214",
	.suite_init	= g214_suite_init,
	.suite_exit	= g214_suite_exit,
	.test_cases	= g214_cases,
};

kunit_test_suites(&g214_suite);

MODULE_DESCRIPTION("xfstests generic/214 over a loopback NFS mount");
MODULE_LICENSE("GPL");
