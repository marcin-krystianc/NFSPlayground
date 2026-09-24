// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/525 over a loopback NFS mount: reads and writes at the
 * top of the 64-bit offset range.
 *
 * Upstream truncates a file to 2^63 - 1, writes a single byte at
 * 2^63 - 2, cycles the mount and reads that byte back. If the filesystem
 * cannot even set such a size it notruns instead.
 *
 * Over NFS this is an encoding test. offset4 and length4 are unsigned
 * 64-bit on the wire while the client works in loff_t, so an offset one
 * below S64_MAX is the value where a sign error, an off-by-one in the
 * s_maxbytes check, or an overflow in offset + length shows up. The
 * generic/308 port writes just under 16 TiB; this one goes to the actual
 * ceiling.
 *
 * Deviations: the mount cycle is the fixture's usual pair -- the page
 * cache is dropped and the byte is read again through the client, and the
 * server's own copy is read through the tmpfs export. O_LARGEFILE is
 * explicit because an in-kernel open does not get force_o_largefile()
 * and would otherwise be capped at 2 GiB (see the 308 port).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G525_ROOT	XFS_MNT "/g525"
#define G525_FILE0	G525_ROOT "/file0"
#define G525_FILE1	G525_ROOT "/file1"
#define G525_SERVER1	XFS_EXPORT "/g525/file1"

#define G525_LEN	((loff_t)S64_MAX)		/* 2^63 - 1 */
#define G525_BIGOFF	((loff_t)S64_MAX - 1)		/* 2^63 - 2 */

static void g525_remove_tree(void *unused)
{
	xfs_unlink(G525_FILE1);
	xfs_unlink(G525_FILE0);
	xfs_rmdir_settled(G525_ROOT);
}

static void a_byte_at_the_top_of_the_offset_range_survives(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos;
	char c;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G525_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g525_remove_tree, NULL),
			0);

	/* upstream's probe: if the size cannot be set, it does not run */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G525_FILE0, "", 0), 0);
	err = xfs_truncate(G525_FILE0, G525_LEN);
	if (err) {
		kunit_skip(test, "the server refused a %lld byte file: %d",
			   G525_LEN, err);
		return;
	}
	KUNIT_ASSERT_EQ(test, xfs_kstat(G525_FILE0, &st), 0);
	KUNIT_ASSERT_EQ_MSG(test, st.size, G525_LEN,
			    "the size came back as %lld", st.size);

	/* one byte at 2^63 - 2 */
	f = filp_open(G525_FILE1, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE,
		      0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	pos = G525_BIGOFF;
	KUNIT_ASSERT_EQ_MSG(test, kernel_write(f, "a", 1, &pos), 1L,
			    "writing at %lld failed", G525_BIGOFF);
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);
	KUNIT_EXPECT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);

	/* read it back through the client, cache-cold */
	pos = G525_BIGOFF;
	c = 0;
	KUNIT_EXPECT_EQ_MSG(test, kernel_read(f, &c, 1, &pos), 1L,
			    "reading back at %lld failed", G525_BIGOFF);
	KUNIT_EXPECT_EQ_MSG(test, c, 'a', "the byte read back as %02x", c);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G525_FILE1, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, G525_BIGOFF + 1,
			    "the file is %lld bytes, expected %lld", st.size,
			    G525_BIGOFF + 1);

	/* and the server's own copy holds it at the same offset */
	c = 0;
	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_read_range(G525_SERVER1, &c, 1, G525_BIGOFF),
			    1L, "the server could not read at %lld",
			    G525_BIGOFF);
	KUNIT_EXPECT_EQ_MSG(test, c, 'a',
			    "the server holds %02x at %lld", c, G525_BIGOFF);
}

static int g525_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g525_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g525_cases[] = {
	KUNIT_CASE(a_byte_at_the_top_of_the_offset_range_survives),
	{}
};

static struct kunit_suite g525_suite = {
	.name		= "xfstests/generic/525",
	.suite_init	= g525_suite_init,
	.suite_exit	= g525_suite_exit,
	.test_cases	= g525_cases,
};

kunit_test_suites(&g525_suite);

MODULE_DESCRIPTION("xfstests generic/525 over a loopback NFS mount");
MODULE_LICENSE("GPL");
