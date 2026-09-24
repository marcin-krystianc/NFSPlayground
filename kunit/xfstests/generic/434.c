// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/434 over a loopback NFS mount: copy_file_range()
 * error checking.
 *
 * Upstream's four refusals, with its golden output:
 *
 *	source offset past EOF		copies nothing, destination empty
 *	destination opened read-only	EBADF
 *	destination opened append-only	EBADF
 *	destination is a device node	EINVAL
 *	destination is a fifo		EINVAL
 *
 * All five are decided before any I/O happens, which over NFS means
 * before nfs4_copy_file_range() can send a COPY: generic_copy_file_checks()
 * rejects the file modes and the non-regular destinations, and the
 * past-EOF case has to come back as a zero-length copy rather than an
 * error. A client that sent the COPY anyway would get a server-side
 * error with a different number, so the exact errno is the assertion.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G434_ROOT	XFS_MNT "/g434"
#define G434_FILE	G434_ROOT "/file"
#define G434_COPY	G434_ROOT "/copy"
#define G434_DEV	G434_ROOT "/dev1"
#define G434_FIFO	G434_ROOT "/fifo"
#define G434_SIZE	1000

static void g434_remove_tree(void *unused)
{
	xfs_unlink(G434_FIFO);
	xfs_unlink(G434_DEV);
	xfs_unlink(G434_COPY);
	xfs_unlink(G434_FILE);
	xfs_rmdir_settled(G434_ROOT);
}

/* copy into path, opened with the given flags */
static ssize_t g434_copy_into(struct kunit *test, const char *path, int flags,
			      loff_t src_off, size_t len)
{
	struct file *src, *dst;
	ssize_t n;

	src = filp_open(G434_FILE, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "source open: %ld",
			       PTR_ERR(src));
	dst = filp_open(path, flags, 0644);
	if (IS_ERR(dst)) {
		filp_close(src, NULL);
		KUNIT_FAIL(test, "%s: open: %ld", path, PTR_ERR(dst));
		return -1;
	}
	n = vfs_copy_file_range(src, src_off, dst, 0, len, 0);
	filp_close(dst, NULL);
	filp_close(src, NULL);
	return n;
}

static void copy_file_range_refuses_what_it_should(struct kunit *test)
{
	struct kstat st;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G434_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g434_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G434_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x61, G434_SIZE);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G434_FILE, buf, G434_SIZE),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G434_DEV, S_IFCHR | 0666, 1, 3), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G434_FIFO, S_IFIFO | 0666, 0, 0), 0);

	/* a source offset past the end copies nothing */
	KUNIT_EXPECT_EQ_MSG(test,
			    g434_copy_into(test, G434_COPY,
					   O_RDWR | O_CREAT | O_TRUNC,
					   G434_SIZE, 100),
			    0L,
			    "a copy from past EOF did not report zero bytes");
	KUNIT_ASSERT_EQ(test, xfs_kstat(G434_COPY, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
			    "the destination is %lld bytes", st.size);

	/* a read-only destination */
	KUNIT_EXPECT_EQ_MSG(test,
			    g434_copy_into(test, G434_COPY, O_RDONLY, 0, 100),
			    (ssize_t)-EBADF,
			    "copying into a read-only file was allowed");

	/* an append-only destination */
	KUNIT_EXPECT_EQ_MSG(test,
			    g434_copy_into(test, G434_COPY,
					   O_WRONLY | O_APPEND, 0, 100),
			    (ssize_t)-EBADF,
			    "copying into an append-only file was allowed");

	/* a device node */
	KUNIT_EXPECT_EQ_MSG(test,
			    g434_copy_into(test, G434_DEV, O_RDWR, 0, 100),
			    (ssize_t)-EINVAL,
			    "copying into a device node was allowed");

	/* and a fifo. O_RDWR, which is what xfs_io uses by default and the
	 * only mode that opens a fifo with nobody at the other end.
	 */
	KUNIT_EXPECT_EQ_MSG(test,
			    g434_copy_into(test, G434_FIFO, O_RDWR, 0, 100),
			    (ssize_t)-EINVAL,
			    "copying into a fifo was allowed");

	/* the destination is still empty after all of that */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G434_COPY, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
			    "the destination ended up %lld bytes", st.size);
}

static int g434_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g434_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g434_cases[] = {
	KUNIT_CASE(copy_file_range_refuses_what_it_should),
	{}
};

static struct kunit_suite g434_suite = {
	.name		= "xfstests/generic/434",
	.suite_init	= g434_suite_init,
	.suite_exit	= g434_suite_exit,
	.test_cases	= g434_cases,
};

kunit_test_suites(&g434_suite);

MODULE_DESCRIPTION("xfstests generic/434 over a loopback NFS mount");
MODULE_LICENSE("GPL");
