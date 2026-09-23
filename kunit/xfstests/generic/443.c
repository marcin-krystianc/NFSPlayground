// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/443 over a loopback NFS mount: writev that faults
 * while walking its iovecs.
 *
 * src/writev_on_pagefault allocates three pages and never touches them,
 * builds three one-byte iovecs -- one per page -- and writevs them. Its
 * own comment says why: "no pre-writing/reading on the buffer before
 * writev, to prevent page prefault from happening, we need it happen at
 * writev time". The kernel copies each segment in turn and takes a page
 * fault part-way through; the write must still report three bytes rather
 * than stopping at the first fault.
 *
 * Over NFS the copy happens inside nfs_write_end()'s window, so a fault
 * on the second segment lands while the client holds a page of the file
 * it is writing. A short write here would not be wrong in itself -- the
 * kernel is allowed to stop and report what it managed -- but it must
 * report a count, not an error, and the bytes it reports must be the
 * ones that arrived.
 *
 * Deviations: the never-touched pages are an anonymous kunit_vm_mmap()
 * mapping rather than malloc, which is the same thing from the kernel's
 * point of view: a mapping with no present PTEs. The port also checks the
 * file contents afterwards, which upstream cannot do because the buffer
 * it writes is uninitialised -- here the pages are known to be zero-filled
 * anonymous memory.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uio.h>

#include "xfstests_nfs_fixture.h"

#define G443_ROOT	XFS_MNT "/g443"
#define G443_FILE	G443_ROOT "/testfile.443"
#define G443_IOVCNT	3		/* upstream's default */

static void g443_remove_tree(void *unused)
{
	xfs_unlink(G443_FILE);
	xfs_rmdir_settled(G443_ROOT);
}

static void writev_faulting_on_its_own_iovecs_writes_them_all(struct kunit *test)
{
	struct iovec iov[G443_IOVCNT];
	unsigned long addr;
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 got[G443_IOVCNT];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G443_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g443_remove_tree, NULL),
			0);

	/* three pages that have never been touched */
	addr = kunit_vm_mmap(test, NULL, 0, G443_IOVCNT * PAGE_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "anonymous mapping failed");

	for (i = 0; i < G443_IOVCNT; i++) {
		iov[i].iov_base = (void __user *)(addr +
						  (unsigned long)i * PAGE_SIZE);
		iov[i].iov_len = 1;
	}

	f = filp_open(G443_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_user_writev(f, iov, G443_IOVCNT, &pos),
			    (ssize_t)G443_IOVCNT,
			    "writev of %d one-byte segments did not write %d bytes",
			    G443_IOVCNT, G443_IOVCNT);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G443_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G443_IOVCNT,
			    "the file is %lld bytes", st.size);

	KUNIT_ASSERT_EQ(test,
			xfs_read_range(G443_FILE, got, G443_IOVCNT, 0),
			(ssize_t)G443_IOVCNT);
	for (i = 0; i < G443_IOVCNT; i++)
		KUNIT_EXPECT_EQ_MSG(test, got[i], 0,
				    "byte %d is %02x; anonymous pages read as zero",
				    i, got[i]);

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G443_IOVCNT * PAGE_SIZE), 0);
}

static int g443_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g443_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g443_cases[] = {
	KUNIT_CASE(writev_faulting_on_its_own_iovecs_writes_them_all),
	{}
};

static struct kunit_suite g443_suite = {
	.name		= "xfstests/generic/443",
	.suite_init	= g443_suite_init,
	.suite_exit	= g443_suite_exit,
	.test_cases	= g443_cases,
};

kunit_test_suites(&g443_suite);

MODULE_DESCRIPTION("xfstests generic/443 over a loopback NFS mount");
MODULE_LICENSE("GPL");
