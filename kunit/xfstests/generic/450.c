// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/450 over a loopback NFS mount: reads at and past EOF.
 *
 * Upstream writes two blocks' worth of data less two sectors, so that EOF
 * lands inside the last block, and then reads five ways -- a sector
 * wholly inside EOF, the block that contains EOF, a sector starting
 * exactly at EOF, the last sector of that block, and somewhere far past
 * the end -- once buffered and once with O_DIRECT. Every read must return
 * either what is there or zero; the bug it was written for made a direct
 * read just past EOF return a negative errno instead of 0.
 *
 * Over NFS a read past EOF is answered by the client without an RPC when
 * it knows the size, and by the server's READ returning eof otherwise, so
 * both paths have to agree that "past the end" is a short read and not an
 * error. The direct path is the one upstream was fixing, and over NFS it
 * is nfs_file_direct_read()'s own EOF clamp rather than the block layer's.
 *
 * Deviations: the block and sector sizes are fixed at 4096 and 512 rather
 * than probed. NFS has no O_DIRECT alignment requirement of its own (the
 * server is asked for whatever range the client is given), so the probe
 * upstream needs to skip unsuitable devices has nothing to say here.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G450_ROOT	XFS_MNT "/g450"
#define G450_FILE	G450_ROOT "/testfile_450"

#define G450_BSIZE	4096
#define G450_SSIZE	512
#define G450_ASIZE	(G450_BSIZE * 2)
#define G450_TSIZE	(G450_ASIZE - G450_SSIZE * 2)	/* EOF inside block 2 */
#define G450_FAR	(G450_ASIZE * 4)

static void g450_remove_tree(void *unused)
{
	xfs_unlink(G450_FILE);
	xfs_rmdir_settled(G450_ROOT);
}

static void g450_read(struct kunit *test, struct file *f, bool direct,
		      loff_t off, size_t len, ssize_t want, const char *what)
{
	u8 *buf = kunit_kzalloc(test, G450_BSIZE, GFP_KERNEL);
	loff_t pos = off;
	ssize_t got;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	got = direct ? xfs_direct_read(f, buf, len, &pos)
		     : kernel_read(f, buf, len, &pos);
	KUNIT_EXPECT_EQ_MSG(test, got, want,
			    "%s read of %s: pread %lld %zu returned %zd, expected %zd",
			    direct ? "direct" : "buffered", what, off, len,
			    got, want);
}

static void g450_one_flavour(struct kunit *test, bool direct)
{
	struct file *f;

	f = filp_open(G450_FILE, O_RDONLY | (direct ? O_DIRECT : 0), 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	g450_read(test, f, direct, 0, G450_SSIZE, G450_SSIZE,
		  "the first sector within EOF");
	g450_read(test, f, direct, G450_BSIZE, G450_BSIZE,
		  G450_TSIZE - G450_BSIZE, "the second block, which holds EOF");
	g450_read(test, f, direct, G450_TSIZE, G450_SSIZE, 0,
		  "a sector starting at EOF");
	g450_read(test, f, direct, G450_TSIZE + G450_SSIZE, G450_SSIZE, 0,
		  "the last sector of the block past EOF");
	g450_read(test, f, direct, G450_FAR, G450_SSIZE, 0,
		  "far past EOF");

	filp_close(f, NULL);
}

static void reads_at_and_past_eof_are_short_not_errors(struct kunit *test)
{
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G450_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g450_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G450_TSIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x5a, G450_TSIZE);

	f = filp_open(G450_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G450_TSIZE, &pos),
			(ssize_t)G450_TSIZE);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	g450_one_flavour(test, false);
	g450_one_flavour(test, true);
}

static int g450_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g450_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g450_cases[] = {
	KUNIT_CASE(reads_at_and_past_eof_are_short_not_errors),
	{}
};

static struct kunit_suite g450_suite = {
	.name		= "xfstests/generic/450",
	.suite_init	= g450_suite_init,
	.suite_exit	= g450_suite_exit,
	.test_cases	= g450_cases,
};

kunit_test_suites(&g450_suite);

MODULE_DESCRIPTION("xfstests generic/450 over a loopback NFS mount");
MODULE_LICENSE("GPL");
