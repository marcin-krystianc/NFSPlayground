// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/246 over a loopback NFS mount: writev whose last
 * segment comes from a mapping.
 *
 * src/t_mmap_writev builds three iovecs -- "aaaaaaaaaa", "bbbbbbbbbb" and
 * a third pointing into a PROT_READ mapping of a second file that holds
 * "cccccccccc" -- and writevs all thirty bytes into a fresh file. Its
 * golden output is the thirty bytes in order. The bug behind it was
 * truncation after a failed write zeroing more than the part that failed:
 * a fault while copying the third segment must not cost the first two.
 *
 * Over NFS the copy into the page cache happens inside nfs_write_end()'s
 * window, and the source is a page of another NFS file that may itself
 * have to be read from the server to service the fault. The port keeps
 * upstream's exact segments and checks the resulting file byte for byte,
 * on both the client and the server.
 *
 * The write goes through xfs_user_writev(): an ITER_IOVEC whose iov_base
 * values are addresses in a kunit_vm_mmap() mapping, which is what
 * upstream's writev(2) passes and what kernel_write() cannot express.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uio.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G246_ROOT	XFS_MNT "/g246"
#define G246_SRC	G246_ROOT "/mmap-writev"
#define G246_DST	G246_ROOT "/mmap-writev.NEW"
#define G246_SERVER_DST	XFS_EXPORT "/g246/mmap-writev.NEW"

#define G246_SEG	10
#define G246_TOTAL	(3 * G246_SEG)
#define G246_MAPLEN	16384		/* upstream maps this much of a 10-byte file */

static void g246_remove_tree(void *unused)
{
	xfs_unlink(G246_DST);
	xfs_unlink(G246_SRC);
	xfs_rmdir_settled(G246_ROOT);
}

static void writev_from_a_mapping_writes_every_segment(struct kunit *test)
{
	static const char cs[G246_SEG] = "cccccccccc";
	static const char as[G246_SEG] = "aaaaaaaaaa";
	static const char bs[G246_SEG] = "bbbbbbbbbb";
	char want[G246_TOTAL], got[G246_TOTAL];
	unsigned long anon, mapped;
	struct file *src, *dst;
	struct iovec iov[3];
	loff_t pos = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G246_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g246_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G246_SRC, cs, G246_SEG), 0);

	src = filp_open(G246_SRC, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "src open: %ld",
			       PTR_ERR(src));
	mapped = kunit_vm_mmap(test, src, 0, G246_MAPLEN, PROT_READ,
			       MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, mapped, 0UL, "mapping the source failed");

	/* the first two segments are ordinary memory upstream; here they
	 * have to be user memory too, so they go in an anonymous mapping
	 */
	anon = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE_MSG(test, anon, 0UL, "anonymous mapping failed");
	KUNIT_ASSERT_EQ(test,
			copy_to_user((void __user *)anon, as, G246_SEG), 0UL);
	KUNIT_ASSERT_EQ(test,
			copy_to_user((void __user *)(anon + 16), bs, G246_SEG),
			0UL);

	dst = filp_open(G246_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(dst), "dst open: %ld",
			       PTR_ERR(dst));

	iov[0].iov_base = (void __user *)anon;
	iov[0].iov_len = G246_SEG;
	iov[1].iov_base = (void __user *)(anon + 16);
	iov[1].iov_len = G246_SEG;
	iov[2].iov_base = (void __user *)mapped;
	iov[2].iov_len = G246_SEG;

	KUNIT_ASSERT_EQ_MSG(test, xfs_user_writev(dst, iov, 3, &pos),
			    (ssize_t)G246_TOTAL, "writev wrote the wrong count");

	filp_close(dst, NULL);
	KUNIT_EXPECT_EQ(test, vm_munmap(mapped, G246_MAPLEN), 0);
	KUNIT_EXPECT_EQ(test, vm_munmap(anon, PAGE_SIZE), 0);
	filp_close(src, NULL);

	memcpy(want, as, G246_SEG);
	memcpy(want + G246_SEG, bs, G246_SEG);
	memcpy(want + 2 * G246_SEG, cs, G246_SEG);

	KUNIT_ASSERT_EQ(test, xfs_read_range(G246_DST, got, G246_TOTAL, 0),
			(ssize_t)G246_TOTAL);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, G246_TOTAL), 0,
			    "client holds %.30s", got);

	KUNIT_ASSERT_EQ(test,
			xfs_read_range(G246_SERVER_DST, got, G246_TOTAL, 0),
			(ssize_t)G246_TOTAL);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, G246_TOTAL), 0,
			    "server holds %.30s", got);
}

static int g246_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g246_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g246_cases[] = {
	KUNIT_CASE(writev_from_a_mapping_writes_every_segment),
	{}
};

static struct kunit_suite g246_suite = {
	.name		= "xfstests/generic/246",
	.suite_init	= g246_suite_init,
	.suite_exit	= g246_suite_exit,
	.test_cases	= g246_cases,
};

kunit_test_suites(&g246_suite);

MODULE_DESCRIPTION("xfstests generic/246 over a loopback NFS mount");
MODULE_LICENSE("GPL");
