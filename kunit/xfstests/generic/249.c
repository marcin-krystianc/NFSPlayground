// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/249 over a loopback NFS mount: sendfile between two
 * files on the same filesystem.
 *
 * Upstream writes a 32 MiB pattern to SRC, fsyncs, copies the whole thing
 * to DST with "xfs_io -c sendfile", fsyncs again, and requires diff to
 * find no difference.
 *
 * sendfile(2) is splice: the read side fills a pipe from the source's
 * page cache (nfs_file_splice_read -> filemap_splice_read) and the write
 * side drains it into the destination (iter_file_splice_write), so the
 * bytes never pass through a userspace buffer. That is a distinct path
 * from read()/write() on both sides of an NFS mount, and it is the one
 * this port exercises: do_splice_direct() is the in-kernel body of
 * sendfile(2).
 *
 * Deviations: 4 MiB rather than 32 MiB, because the fixture's export is a
 * 64 MiB tmpfs and both files live on it. The comparison is byte-exact
 * like diff's, and the destination is also compared through the server's
 * own copy.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/splice.h>

#include "xfstests_nfs_fixture.h"

#define G249_ROOT	XFS_MNT "/g249"
#define G249_SRC	G249_ROOT "/249.src"
#define G249_DST	G249_ROOT "/249.dst"
#define G249_SERVER_DST	XFS_EXPORT "/g249/249.dst"

#define G249_SIZE	(4 * 1024 * 1024)	/* upstream: 32768k */
#define G249_CHUNK	65536
#define G249_PATTERN	0xa5a55a5a		/* upstream's -S value */

static void g249_remove_tree(void *unused)
{
	xfs_unlink(G249_DST);
	xfs_unlink(G249_SRC);
	xfs_rmdir_settled(G249_ROOT);
}

static void g249_compare(struct kunit *test, const char *path, u32 *buf,
			 const char *which)
{
	struct file *f;
	loff_t pos = 0;
	int i;

	f = filp_open(path, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", which,
			       PTR_ERR(f));

	while (pos < G249_SIZE) {
		loff_t at = pos;

		KUNIT_ASSERT_EQ_MSG(test, kernel_read(f, buf, G249_CHUNK, &pos),
				    (ssize_t)G249_CHUNK,
				    "%s: read at %lld failed", which, at);
		for (i = 0; i < G249_CHUNK / sizeof(u32); i++)
			if (buf[i] != G249_PATTERN) {
				KUNIT_FAIL(test,
					   "%s: word at offset %lld is %08x",
					   which,
					   at + i * (loff_t)sizeof(u32),
					   buf[i]);
				filp_close(f, NULL);
				return;
			}
	}
	filp_close(f, NULL);
}

static void sendfile_copies_every_byte(struct kunit *test)
{
	struct file *src, *dst;
	loff_t pos = 0, opos = 0;
	struct kstat st;
	ssize_t copied;
	u32 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G249_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g249_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G249_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G249_CHUNK / sizeof(u32); i++)
		buf[i] = G249_PATTERN;

	src = filp_open(G249_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(src), "src open: %ld",
			       PTR_ERR(src));
	while (pos < G249_SIZE)
		KUNIT_ASSERT_EQ(test, kernel_write(src, buf, G249_CHUNK, &pos),
				(ssize_t)G249_CHUNK);
	KUNIT_ASSERT_EQ(test, vfs_fsync(src, 0), 0);

	dst = filp_open(G249_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(dst), "dst open: %ld",
			       PTR_ERR(dst));

	/* sendfile(2)'s body; it may copy less than asked, so loop */
	pos = 0;
	while (opos < G249_SIZE) {
		copied = do_splice_direct(src, &pos, dst, &opos,
					  G249_SIZE - opos, 0);
		KUNIT_ASSERT_GT_MSG(test, copied, 0,
				    "sendfile stopped at %lld with %zd",
				    opos, copied);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(dst, 0), 0);
	filp_close(dst, NULL);
	filp_close(src, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G249_DST, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G249_SIZE,
			    "the copy is %lld bytes", st.size);

	g249_compare(test, G249_DST, buf, "client");
	g249_compare(test, G249_SERVER_DST, buf, "server");
}

static int g249_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g249_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g249_cases[] = {
	KUNIT_CASE_SLOW(sendfile_copies_every_byte),
	{}
};

static struct kunit_suite g249_suite = {
	.name		= "xfstests/generic/249",
	.suite_init	= g249_suite_init,
	.suite_exit	= g249_suite_exit,
	.test_cases	= g249_cases,
};

kunit_test_suites(&g249_suite);

MODULE_DESCRIPTION("xfstests generic/249 over a loopback NFS mount");
MODULE_LICENSE("GPL");
