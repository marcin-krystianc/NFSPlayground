// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/708 over a loopback NFS mount: a direct write whose
 * buffer is a mapping, only part of which is faulted in.
 *
 * Upstream writes a 2 MiB source file with xfs_io. src/dio-buf-fault
 * opens it, maps it PROT_READ, MAP_PRIVATE, touches only its first page,
 * and writes the whole mapping with O_DIRECT into a second file opened
 * O_CREAT | O_TRUNC | O_WRONLY, continuing after short writes. Last,
 * the test diffs the two files. The write has to fault the rest of the
 * source in as it goes, so it is a direct write that completes partially
 * and then continues -- the path btrfs got wrong in commit b73a6fd1b1ef
 * ("btrfs: split partial dio bios before submit").
 *
 * Over NFS the equivalent is nfs_direct_write() extracting pages from a
 * user-backed iterator whose later pages are not present: the extraction
 * faults them in one chunk at a time, and each chunk becomes its own
 * WRITE. A filesystem that miscounts the short first chunk loses the
 * rest.
 *
 * Deviation: the source holds a positional pattern rather than
 * upstream's 0xcd, so a chunk written to the wrong offset shows in the
 * compare. Not in upstream: the destination is also checked on the
 * server.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G708_ROOT	XFS_MNT "/g708"
#define G708_SRC	G708_ROOT "/dio-buf-fault.src"
#define G708_DST	G708_ROOT "/dio-buf-fault.dst"
#define G708_SERVER_DST	XFS_EXPORT "/g708/dio-buf-fault.dst"

#define G708_SIZE	(2 * 1024 * 1024)
#define G708_CHUNK	4096

static void g708_remove_tree(void *unused)
{
	xfs_settle_fput();
	xfs_unlink(G708_DST);
	xfs_unlink(G708_SRC);
	xfs_rmdir_settled(G708_ROOT);
}

static u8 g708_byte(loff_t off)
{
	return (u8)(off >> 12) ^ (u8)off;
}

static void g708_verify(struct kunit *test, const char *path, u8 *buf,
			const char *which)
{
	loff_t off;
	int i;

	for (off = 0; off < G708_SIZE; off += G708_CHUNK) {
		ssize_t n = xfs_read_range(path, buf, G708_CHUNK, off);

		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G708_CHUNK,
				    "%s: read at %lld returned %zd", which,
				    off, n);
		for (i = 0; i < G708_CHUNK; i++)
			if (buf[i] != g708_byte(off + i)) {
				KUNIT_FAIL(test,
					   "%s: byte %lld is %02x, expected %02x",
					   which, off + i, buf[i],
					   g708_byte(off + i));
				return;
			}
	}
}

static void a_direct_write_from_a_partly_faulted_mapping(struct kunit *test)
{
	unsigned long addr;
	struct file *src, *dst;
	struct kstat st;
	loff_t pos;
	u8 *buf, one;
	ssize_t n;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G708_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g708_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G708_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* xfs_io -fc "pwrite -q 0 2m" $src, with a positional pattern */
	src = filp_open(G708_SRC, O_RDWR | O_CREAT, 0600);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "src create: %ld",
			       PTR_ERR(src));
	for (pos = 0; pos < G708_SIZE; ) {
		loff_t at = pos;

		for (i = 0; i < G708_CHUNK; i++)
			buf[i] = g708_byte(at + i);
		KUNIT_ASSERT_EQ(test, kernel_write(src, buf, G708_CHUNK, &pos),
				(ssize_t)G708_CHUNK);
	}
	filp_close(src, NULL);

	/* prep_mmap_buffer */
	src = filp_open(G708_SRC, O_RDWR, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "src open: %ld",
			       PTR_ERR(src));
	KUNIT_ASSERT_EQ(test, vfs_getattr(&src->f_path, &st, STATX_SIZE,
					  AT_STATX_SYNC_AS_STAT), 0);
	KUNIT_ASSERT_EQ(test, st.size, (loff_t)G708_SIZE);
	addr = kunit_vm_mmap(test, src, 0, st.size, PROT_READ, MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	/* touch only the first page, as upstream does */
	KUNIT_ASSERT_EQ(test, copy_from_user(&one, (void __user *)addr, 1),
			0UL);

	/* do_dio */
	dst = filp_open(G708_DST, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT,
			0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(dst), "dst open: %ld",
			       PTR_ERR(dst));

	pos = 0;
	while (pos < G708_SIZE) {
		loff_t at = pos;

		n = xfs_user_rw(dst, (void __user *)(addr + pos),
				G708_SIZE - pos, &pos, true, true);
		KUNIT_ASSERT_GT_MSG(test, n, 0,
				    "the direct write stopped at %lld with %zd",
				    at, n);
	}
	filp_close(dst, NULL);

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G708_SIZE), 0);
	filp_close(src, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G708_DST, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G708_SIZE,
			    "the copy is %lld bytes", st.size);

	/* diff $src $dst */
	g708_verify(test, G708_SRC, buf, "src");
	g708_verify(test, G708_DST, buf, "dst");
	/* not upstream */
	g708_verify(test, G708_SERVER_DST, buf, "server dst");
}

static int g708_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g708_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g708_cases[] = {
	KUNIT_CASE_SLOW(a_direct_write_from_a_partly_faulted_mapping),
	{}
};

static struct kunit_suite g708_suite = {
	.name		= "xfstests/generic/708",
	.suite_init	= g708_suite_init,
	.suite_exit	= g708_suite_exit,
	.test_cases	= g708_cases,
};

kunit_test_suites(&g708_suite);

MODULE_DESCRIPTION("xfstests generic/708 over a loopback NFS mount");
MODULE_LICENSE("GPL");
