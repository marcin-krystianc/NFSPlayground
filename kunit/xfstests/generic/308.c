// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/308 over a loopback NFS mount: writes at the maximum
 * file offset.
 *
 * Upstream is a regression test for ext4 commit f17722f "ext4: Fix max file
 * size and logical block counting of extent format file". It writes one
 * block at the second-to-last representable block and then one block at the
 * last:
 *
 *	offset=$(((2**32 - 2) * $block_size))	# then (2**32 - 1)
 *	xfs_io -f -c "pwrite $offset $block_size" -c fsync $testfile
 *
 * Both writes have their output thrown away, because the outcome does not
 * matter: on the unpatched kernel the second one panicked, on the patched
 * one it returns EFBIG. "Got here without hitting BUG_ON(), test passed".
 *
 * The offsets are the point, so they are computed the same way -- from the
 * filesystem's own block size, which for the NFS mount comes back in
 * statfs(2) exactly as upstream's _get_block_size reads it. At a 4 KiB block
 * that is a write at just under 16 TiB, sparse: the tmpfs export allocates
 * the two written pages and nothing for the hole.
 *
 * A write that is refused must be refused cleanly (EFBIG is the documented
 * answer), and one that succeeds must read back -- otherwise "no crash"
 * would also be satisfied by silently dropping the data.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/statfs.h>

#include "xfstests_nfs_fixture.h"

#define G308_ROOT	XFS_MNT "/g308"
#define G308_FILE	G308_ROOT "/testfile.308"

static void g308_remove_tree(void *unused)
{
	xfs_unlink(G308_FILE);
	xfs_rmdir(G308_ROOT);
}

/*
 * One of upstream's two writes. Returns the write's result so the caller can
 * report it; a refusal is a legitimate outcome, a corrupt success is not.
 */
static ssize_t g308_write_block(struct kunit *test, struct file *f, loff_t off,
				size_t bs, u8 stamp, const char *what)
{
	u8 *buf, *rd;
	loff_t pos = off;
	ssize_t n;

	buf = kunit_kmalloc(test, bs, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, stamp, bs);

	n = kernel_write(f, buf, bs, &pos);
	if (n < 0) {
		/*
		 * The patched behaviour: refused, and named. Anything other
		 * than the documented EFBIG would be a new failure mode.
		 */
		KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)-EFBIG,
				    "%s at %lld failed with %zd, expected EFBIG or success",
				    what, off, n);
		return n;
	}

	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)bs,
			    "%s at %lld wrote %zd of %zu bytes", what, off, n,
			    bs);
	KUNIT_EXPECT_EQ_MSG(test, vfs_fsync(f, 0), 0, "%s: fsync", what);

	/* it claimed success, so the block has to be there */
	rd = kunit_kzalloc(test, bs, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rd);
	pos = off;
	KUNIT_EXPECT_EQ_MSG(test, kernel_read(f, rd, bs, &pos), (ssize_t)bs,
			    "%s: the accepted block at %lld will not read back",
			    what, off);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, rd, bs), 0,
			    "%s: the block at %lld came back different", what,
			    off);
	return n;
}

static void writes_at_the_maximum_offset_do_not_break(struct kunit *test)
{
	struct kstatfs sfs;
	struct kstat st;
	struct file *f;
	loff_t bs;
	ssize_t last;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G308_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g308_remove_tree, NULL),
			0);

	/* _get_block_size's answer */
	KUNIT_ASSERT_EQ(test, xfs_statfs(G308_ROOT, &sfs), 0);
	KUNIT_ASSERT_GT(test, sfs.f_bsize, 0L);
	bs = sfs.f_bsize;

	/*
	 * O_LARGEFILE by hand: userspace gets it from force_o_largefile(),
	 * in-kernel filp_open() does not, and without it generic_write_checks
	 * stops at MAX_NON_LFS (2 GiB) long before the offset under test.
	 */
	f = filp_open(G308_FILE, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* the block before the maximum offset */
	g308_write_block(test, f, ((loff_t)0x100000000ULL - 2) * bs, bs, 0x5C,
			 "write below the max offset");

	/* and the block at it -- the one that used to panic */
	last = g308_write_block(test, f, ((loff_t)0x100000000ULL - 1) * bs, bs,
				0x5D, "write at the max offset");

	KUNIT_ASSERT_EQ(test, xfs_kstat(G308_FILE, &st), 0);
	if (last > 0)
		KUNIT_EXPECT_EQ_MSG(test, st.size,
				    ((loff_t)0x100000000ULL - 1) * bs + (loff_t)bs,
				    "the accepted write left the size at %lld",
				    st.size);
	else
		KUNIT_EXPECT_EQ_MSG(test, st.size,
				    ((loff_t)0x100000000ULL - 2) * bs + (loff_t)bs,
				    "the refused write changed the size to %lld",
				    st.size);

	filp_close(f, NULL);
}

static int g308_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g308_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g308_cases[] = {
	KUNIT_CASE(writes_at_the_maximum_offset_do_not_break),
	{}
};

static struct kunit_suite g308_suite = {
	.name		= "xfstests/generic/308",
	.suite_init	= g308_suite_init,
	.suite_exit	= g308_suite_exit,
	.test_cases	= g308_cases,
};

kunit_test_suites(&g308_suite);

MODULE_DESCRIPTION("xfstests generic/308 over a loopback NFS mount");
MODULE_LICENSE("GPL");
