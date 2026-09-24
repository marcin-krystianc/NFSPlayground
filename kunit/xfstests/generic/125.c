// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/125 over a loopback NFS mount: a buffered write, a
 * truncate inside the same block, and direct reads of the result.
 *
 * Upstream runs two programs. src/ftrunc, as an unprivileged user, opens
 * a file read-write, ftruncates it to 1000 bytes and unlinks it. Then
 * src/trunc writes 4096 bytes of 0x01 with O_DIRECT, reopens the file
 * buffered and writes 4096 bytes of 0x02 without flushing, truncates to
 * 1000 bytes, fdatasyncs, and then re-reads the file with O_DIRECT for a
 * minute, requiring the first hundred bytes to be 0x02 every time.
 *
 * Its comment says what it is looking for: "during truncate server may
 * have read/modified/written last block". That is an NFS sentence -- the
 * test is in the "pnfs" group upstream -- and it is exactly what this
 * fixture can exercise. The truncate is a SETATTR while a page of 0x02
 * is still dirty on the client; if the client writes that page back as a
 * whole block after the SETATTR, or lets the server rewrite the tail from
 * stale data, the direct reads see 0x01 again.
 *
 * Deviations: 200 direct reads rather than sixty seconds of them, and the
 * unprivileged half runs with xfs_switch_creds() rather than as a
 * separate user process.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G125_ROOT	XFS_MNT "/g125"
#define G125_DIR	G125_ROOT "/ftrunc"
#define G125_FILE	G125_DIR "/ftrunc.tmp"

#define G125_UID	1000
#define G125_GID	1000
#define G125_BUFSZ	4096
#define G125_TRUNC	1000
#define G125_READS	200

static void g125_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G125_FILE);
	xfs_rmdir_settled(G125_DIR);
	xfs_rmdir_settled(G125_ROOT);
}

/* src/ftrunc: as a mortal user, ftruncate an open file and unlink it */
static void an_unprivileged_ftruncate_and_unlink(struct kunit *test)
{
	struct file *f;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G125_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g125_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G125_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G125_FILE, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G125_DIR, 0777), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G125_FILE, 0666), 0);

	KUNIT_ASSERT_EQ(test, xfs_switch_creds(G125_UID, G125_GID), 0);
	f = filp_open(G125_FILE, O_CREAT | O_RDWR, 0400);
	if (IS_ERR(f)) {
		xfs_restore_creds();
		KUNIT_FAIL(test, "open as an unprivileged user: %ld",
			   PTR_ERR(f));
		return;
	}
	err = xfs_ftruncate(f, G125_TRUNC);
	filp_close(f, NULL);
	if (!err)
		err = xfs_unlink(G125_FILE);
	xfs_restore_creds();

	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "ftruncate/unlink as the file's owner failed: %d",
			    err);
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G125_FILE),
			       "the file is still there after unlink");
}

/* src/trunc: the truncate that lands inside a dirty block */
static void direct_reads_after_a_truncate_see_the_new_data(struct kunit *test)
{
	struct file *f;
	loff_t pos;
	u8 *buf;
	int i, j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G125_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g125_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G125_DIR), 0);

	buf = kunit_kmalloc(test, G125_BUFSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* 0x01 straight to the server */
	memset(buf, 1, G125_BUFSZ);
	f = filp_open(G125_FILE, O_CREAT | O_RDWR | O_DIRECT, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct open: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_ASSERT_EQ(test, xfs_direct_write(f, buf, G125_BUFSZ, &pos),
			(ssize_t)G125_BUFSZ);
	filp_close(f, NULL);

	/* 0x02 buffered, then truncate into that block, then fdatasync */
	memset(buf, 2, G125_BUFSZ);
	f = filp_open(G125_FILE, O_CREAT | O_RDWR, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "buffered open: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G125_BUFSZ, &pos),
			(ssize_t)G125_BUFSZ);
	KUNIT_ASSERT_EQ(test, xfs_ftruncate(f, G125_TRUNC), 0);
	KUNIT_ASSERT_EQ_MSG(test, vfs_fsync(f, 1), 0, "fdatasync failed");
	filp_close(f, NULL);

	/* and read it back, direct, over and over */
	f = filp_open(G125_FILE, O_CREAT | O_RDWR | O_DIRECT, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct reopen: %ld",
			       PTR_ERR(f));
	for (i = 0; i < G125_READS; i++) {
		ssize_t n;

		memset(buf, 0, G125_BUFSZ);
		pos = 0;
		n = xfs_direct_read(f, buf, G125_BUFSZ, &pos);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G125_TRUNC,
				    "read %d returned %zd, expected %d", i, n,
				    G125_TRUNC);
		for (j = 0; j < 100; j++)
			if (buf[j] != 2) {
				KUNIT_FAIL(test,
					   "read %d: byte %d is %d, expected 2",
					   i, j, buf[j]);
				filp_close(f, NULL);
				return;
			}
	}
	filp_close(f, NULL);
}

static int g125_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g125_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g125_cases[] = {
	KUNIT_CASE(an_unprivileged_ftruncate_and_unlink),
	KUNIT_CASE_SLOW(direct_reads_after_a_truncate_see_the_new_data),
	{}
};

static struct kunit_suite g125_suite = {
	.name		= "xfstests/generic/125",
	.suite_init	= g125_suite_init,
	.suite_exit	= g125_suite_exit,
	.test_cases	= g125_cases,
};

kunit_test_suites(&g125_suite);

MODULE_DESCRIPTION("xfstests generic/125 over a loopback NFS mount");
MODULE_LICENSE("GPL");
