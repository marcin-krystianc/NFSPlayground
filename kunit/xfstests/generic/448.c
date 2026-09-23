// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/448 over a loopback NFS mount: SEEK_HOLE and SEEK_DATA
 * with negative offsets.
 *
 * Upstream runs seek_sanity_test case 18 ("Test file with negative
 * SEEK_{HOLE,DATA} offsets"): on an empty file, seek to -1 and to
 * LLONG_MIN with both whences, all four of which must fail rather than
 * return a nonsense offset or trip over an unchecked subtraction. It then
 * checks dmesg, because the original bug was a warning in the kernel.
 *
 * The expected errno is ENXIO, not EINVAL: seek_sanity_test's do_lseek()
 * requires errno == ENXIO for every case whose expected offset is -1.
 * That is what this mount produces, and by a route worth recording --
 * nfs4_file_llseek() hands SEEK_HOLE and SEEK_DATA straight to
 * nfs42_proc_llseek(), so the negative offset is encoded as an unsigned
 * offset4 and goes to the server, which answers NFS4ERR_NXIO because it
 * is past EOF. The offset is never rejected locally; the right errno
 * comes back for a different reason than on a local filesystem.
 *
 * The port also seeks to a valid offset afterwards, so a client that
 * rejected everything would not pass by accident.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G448_ROOT	XFS_MNT "/g448"
#define G448_FILE	G448_ROOT "/seek_sanity_testfile_448"

static void g448_remove_tree(void *unused)
{
	xfs_unlink(G448_FILE);
	xfs_rmdir_settled(G448_ROOT);
}

static void g448_check(struct kunit *test, struct file *f, int whence,
		       loff_t off, const char *what)
{
	loff_t got = vfs_llseek(f, off, whence);

	KUNIT_EXPECT_EQ_MSG(test, got, (loff_t)-ENXIO,
			    "SEEK_%s to %s returned %lld, expected -ENXIO",
			    whence == SEEK_HOLE ? "HOLE" : "DATA", what, got);
}

static void negative_seek_offsets_are_rejected(struct kunit *test)
{
	struct file *f;
	loff_t got, pos = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G448_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g448_remove_tree, NULL),
			0);

	f = filp_open(G448_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	/* upstream's four sub-cases, on a zero-length file */
	g448_check(test, f, SEEK_HOLE, -1, "-1");
	g448_check(test, f, SEEK_DATA, -1, "-1");
	g448_check(test, f, SEEK_HOLE, LLONG_MIN, "LLONG_MIN");
	g448_check(test, f, SEEK_DATA, LLONG_MIN, "LLONG_MIN");

	/* and a legal seek on the same file still works. The file is given
	 * real data first: SEEK_DATA on a wholly sparse file is ENXIO, not 0.
	 */
	buf = kunit_kzalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 4096, &pos), 4096L);
	got = vfs_llseek(f, 0, SEEK_DATA);
	KUNIT_EXPECT_EQ_MSG(test, got, 0LL,
			    "SEEK_DATA to 0 on a 4096-byte file returned %lld",
			    got);
	got = vfs_llseek(f, 0, SEEK_HOLE);
	KUNIT_EXPECT_EQ_MSG(test, got, 4096LL,
			    "SEEK_HOLE to 0 on a 4096-byte file returned %lld",
			    got);

	filp_close(f, NULL);
}

static int g448_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g448_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g448_cases[] = {
	KUNIT_CASE(negative_seek_offsets_are_rejected),
	{}
};

static struct kunit_suite g448_suite = {
	.name		= "xfstests/generic/448",
	.suite_init	= g448_suite_init,
	.suite_exit	= g448_suite_exit,
	.test_cases	= g448_cases,
};

kunit_test_suites(&g448_suite);

MODULE_DESCRIPTION("xfstests generic/448 over a loopback NFS mount");
MODULE_LICENSE("GPL");
