// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/047 over a loopback NFS mount: per-file fsync durability
 * in bulk.
 *
 * Upstream is one of a family (043-049) called "test for NULL files
 * problem": write many small files, force them out, shut the filesystem down
 * with the XFS shutdown ioctl, remount, and check that no file has a
 * non-zero size but no extents -- the failure being a file that comes back
 * the right length full of NULs. 047 is the fsync member: each file is
 * written and fsynced individually before the shutdown.
 *
 * The shutdown ioctl has no NFS equivalent, so the crash half is gone, and
 * "non-zero size but no extents" is a fiemap question that NFSv4.2 cannot
 * answer. What is left is still a real durability check over NFS, and it is
 * shaped differently from the other ports: fsync on an NFS file is
 * nfs_file_fsync() -> nfs_wb_all() plus a COMMIT, and this is the only case
 * in the set that drives it across **many files in bulk** rather than one.
 * A client that dropped or mis-tagged a single COMMIT among hundreds shows
 * up here and nowhere else.
 *
 * The check upstream performs after remounting is done here by reading
 * through the tmpfs export -- the server's own bytes, which is what the
 * COMMIT was supposed to guarantee. Upstream's three failure messages map
 * directly: a missing file, a wrong size, and a file of the right size whose
 * content is NULs.
 *
 * Deviation: upstream's 999 files become G047_FILES, because each one here
 * is a real OPEN/WRITE/COMMIT/CLOSE round trip against a loopback server
 * rather than a page-cache write, and the count is not what the test is
 * about. The 32K size and 0xff fill are upstream's.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G047_ROOT	XFS_MNT "/g047"
#define G047_EXPORT	XFS_EXPORT "/g047"

#define G047_FILES	150		/* upstream's 999, scaled */
#define G047_SIZE	(32 * 1024)	/* upstream's 32k */
#define G047_FILL	0xff		/* upstream's -S 0xff */

static void g047_remove_tree(void *unused)
{
	char name[80];
	int i;

	for (i = 1; i <= G047_FILES; i++) {
		snprintf(name, sizeof(name), G047_ROOT "/%d", i);
		xfs_unlink(name);
	}
	xfs_rmdir_settled(G047_ROOT);
}

static void fsynced_files_are_complete_on_the_server(struct kunit *test)
{
	char name[80];
	u8 *buf, *got;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G047_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g047_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	memset(buf, G047_FILL, PAGE_SIZE);

	/* "create files and fsync them" */
	for (i = 1; i <= G047_FILES; i++) {
		struct file *f;
		loff_t off;
		int err = 0;

		snprintf(name, sizeof(name), G047_ROOT "/%d", i);
		f = filp_open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
				       "error creating file %s: %ld", name,
				       PTR_ERR(f));

		/*
		 * No assertions while the file is open: an aborted case leaks
		 * the struct file and wedges the mount for every suite after
		 * this one. (Same discipline as generic/102.)
		 */
		for (off = 0; off < G047_SIZE && !err; off += PAGE_SIZE) {
			loff_t pos = off;
			ssize_t n = kernel_write(f, buf, PAGE_SIZE, &pos);

			if (n != PAGE_SIZE)
				err = (n < 0) ? (int)n : -EIO;
		}
		if (!err)
			err = vfs_fsync(f, 0);
		filp_close(f, NULL);
		KUNIT_ASSERT_EQ_MSG(test, err, 0,
				    "error creating/writing file %s: %d", name,
				    err);
	}

	/* upstream's _check_files, against the server's own copy */
	for (i = 1; i <= G047_FILES; i++) {
		struct kstat st;
		loff_t off;

		snprintf(name, sizeof(name), G047_ROOT "/%d", i);
		KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(name),
				      "file %s missing - fsync failed", name);
		KUNIT_ASSERT_EQ(test, xfs_kstat(name, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)G047_SIZE,
				    "file %s has incorrect size %lld - fsync failed",
				    name, st.size);

		snprintf(name, sizeof(name), G047_EXPORT "/%d", i);
		for (off = 0; off < G047_SIZE; off += PAGE_SIZE) {
			ssize_t r = xfs_read_range(name, got, PAGE_SIZE, off);
			size_t j;

			KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)PAGE_SIZE,
					    "server read of %s at %lld returned %zd",
					    name, off, r);
			for (j = 0; j < PAGE_SIZE; j++)
				if (got[j] != G047_FILL) {
					/* upstream's "non-zero size but no extents" */
					KUNIT_FAIL(test,
						   "corrupt file %s - byte %lld is %02x, expected %02x (fsync did not reach the server)",
						   name, off + (loff_t)j,
						   got[j], G047_FILL);
					return;
				}
		}
	}
}

static int g047_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g047_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g047_cases[] = {
	KUNIT_CASE_SLOW(fsynced_files_are_complete_on_the_server),
	{}
};

static struct kunit_suite g047_suite = {
	.name		= "xfstests/generic/047",
	.suite_init	= g047_suite_init,
	.suite_exit	= g047_suite_exit,
	.test_cases	= g047_cases,
};

kunit_test_suites(&g047_suite);

MODULE_DESCRIPTION("xfstests generic/047 over a loopback NFS mount");
MODULE_LICENSE("GPL");
