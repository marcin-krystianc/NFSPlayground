// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/680 over a loopback NFS mount: the Dirty Pipe
 * vulnerability (CVE-2022-0847).
 *
 * src/splice2pipe fills a pipe and drains it again, so that every
 * pipe_buffer left behind still has PIPE_BUF_FLAG_CAN_MERGE set; splices
 * a single byte of a read-only file into the now-empty pipe, which adds a
 * reference to the file's page cache page without initialising the
 * buffer's flags; and then writes into the pipe. With the bug, that write
 * merged into the page cache page and so modified a file the caller only
 * had open for reading. Commit 9d2231c5d74e ("lib/iov_iter: initialize
 * 'flags' in new pipe_buffer") fixed it. Upstream runs it as root and as
 * an unprivileged user and hexdumps the file both times.
 *
 * Over NFS the page the splice pins is an NFS page cache page, and a
 * merged write into it would be picked up by writeback and sent to the
 * server as a legitimate WRITE -- so the port checks the file through the
 * client and then through the server's own copy, after flushing, rather
 * than only hexdumping it.
 *
 * Deviations: the pipe is created with create_pipe_files() and the splice
 * is vfs_splice_read(), which are what the syscalls reduce to; the
 * unprivileged half runs with xfs_switch_creds().
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pipe_fs_i.h>
#include <linux/splice.h>

#include "xfstests_nfs_fixture.h"

#define G680_ROOT	XFS_MNT "/g680"
#define G680_FILE	G680_ROOT "/testfile.680"
#define G680_SERVER	XFS_EXPORT "/g680/testfile.680"

#define G680_SIZE	4096
#define G680_UID	1000
#define G680_GID	1000
#define G680_DATA	"AAAAAAAABBBBBBBB"
#define G680_PIPESZ	(64 * 1024)

static void g680_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G680_FILE);
	xfs_rmdir_settled(G680_ROOT);
}

/* fill the pipe and drain it: every pipe_buffer keeps its stale flags */
static int g680_prepare_pipe(struct kunit *test, struct file **files, u8 *buf)
{
	loff_t pos = 0;
	int left;

	memset(buf, 0, PAGE_SIZE);
	for (left = G680_PIPESZ; left > 0; left -= PAGE_SIZE) {
		ssize_t n = kernel_write(files[1], buf, PAGE_SIZE, &pos);

		if (n != PAGE_SIZE)
			return n < 0 ? (int)n : -EIO;
	}
	pos = 0;
	for (left = G680_PIPESZ; left > 0; left -= PAGE_SIZE) {
		ssize_t n = kernel_read(files[0], buf, PAGE_SIZE, &pos);

		if (n != PAGE_SIZE)
			return n < 0 ? (int)n : -EIO;
	}
	return 0;
}

static void g680_expect_unchanged(struct kunit *test, const char *path,
				  u8 *buf, const char *which)
{
	int i;

	KUNIT_ASSERT_EQ(test, xfs_read_range(path, buf, G680_SIZE, 0),
			(ssize_t)G680_SIZE);
	for (i = 0; i < G680_SIZE; i++)
		if (buf[i] != 0xff) {
			KUNIT_FAIL(test,
				   "%s: byte %d is %02x -- the read-only file was modified",
				   which, i, buf[i]);
			return;
		}
}

static void g680_try(struct kunit *test, bool as_user)
{
	const char *what = as_user ? "unprivileged" : "privileged";
	struct file *files[2], *f;
	loff_t pos = 0;
	u8 *buf;
	int err;

	buf = kunit_kmalloc(test, G680_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xff, G680_SIZE);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G680_FILE, buf, G680_SIZE),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G680_FILE, 0644), 0);

	if (as_user)
		KUNIT_ASSERT_EQ(test, xfs_switch_creds(G680_UID, G680_GID), 0);

	/* read-only, on purpose */
	f = filp_open(G680_FILE, O_RDONLY, 0);
	if (IS_ERR(f)) {
		if (as_user)
			xfs_restore_creds();
		KUNIT_FAIL(test, "%s: open: %ld", what, PTR_ERR(f));
		return;
	}

	err = create_pipe_files(files, 0);
	if (err) {
		filp_close(f, NULL);
		if (as_user)
			xfs_restore_creds();
		KUNIT_FAIL(test, "%s: create_pipe_files: %d", what, err);
		return;
	}

	err = g680_prepare_pipe(test, files, buf);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "%s: preparing the pipe: %d", what,
			    err);

	if (!err) {
		/* splice2pipe $file 1: one byte from offset 1 - 1 */
		pos = 0;
		KUNIT_EXPECT_EQ_MSG(test,
				    vfs_splice_read(f, &pos,
						    files[0]->private_data, 1,
						    0),
				    1L, "%s: the splice did not move a byte",
				    what);

		/* and the write that must not reach the file */
		pos = 0;
		KUNIT_EXPECT_EQ_MSG(test,
				    kernel_write(files[1], G680_DATA,
						 strlen(G680_DATA), &pos),
				    (ssize_t)strlen(G680_DATA),
				    "%s: the pipe write failed", what);
	}

	fput(files[0]);
	fput(files[1]);
	filp_close(f, NULL);
	if (as_user)
		xfs_restore_creds();

	g680_expect_unchanged(test, G680_FILE, buf, what);
	KUNIT_EXPECT_EQ(test, xfs_fsync_path(G680_FILE), 0);
	g680_expect_unchanged(test, G680_SERVER, buf, what);
}

static void a_pipe_write_cannot_modify_a_read_only_file(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G680_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g680_remove_tree, NULL),
			0);

	g680_try(test, false);
	g680_try(test, true);
}

static int g680_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g680_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g680_cases[] = {
	KUNIT_CASE(a_pipe_write_cannot_modify_a_read_only_file),
	{}
};

static struct kunit_suite g680_suite = {
	.name		= "xfstests/generic/680",
	.suite_init	= g680_suite_init,
	.suite_exit	= g680_suite_exit,
	.test_cases	= g680_cases,
};

kunit_test_suites(&g680_suite);

MODULE_DESCRIPTION("xfstests generic/680 over a loopback NFS mount");
MODULE_LICENSE("GPL");
