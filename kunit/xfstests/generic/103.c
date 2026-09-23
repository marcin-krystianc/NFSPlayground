// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/103 over a loopback NFS mount: large xattr values set
 * at ENOSPC, then unlinked.
 *
 * Upstream creates 64 files, generates a 64k attribute value, fills the
 * filesystem with a fallocated file leaving ~512k free, sets user.test to
 * that 64k value on each of the 64 files (expecting some of them to start
 * failing with ENOSPC), and then removes the files. Its golden output is
 * "Silence is golden": the assertion is that the attribute-fork teardown
 * on unlink does not blow up when the attributes were only partly
 * written. It was written for an XFS regression and runs anywhere.
 *
 * Over NFSv4.2 the shape survives: each set is a SETXATTR (RFC 8276) of a
 * 64k value, the filling is an ALLOCATE, and the removal is a REMOVE per
 * file. What is being checked here is that a SETXATTR refused for lack of
 * space leaves the file removable and the space recoverable, and that a
 * refusal arrives as ENOSPC rather than as a broken connection.
 *
 * Deviations: whether the server can still store the attribute on a full
 * filesystem is the server's business -- tmpfs keeps xattrs in kernel
 * memory, not in the space the export accounts for -- so both outcomes
 * are accepted per file, exactly as upstream accepts both (it sends its
 * setfattr errors to $seqres.full). What is asserted is what upstream
 * asserts by exiting 0: every file is removable afterwards, and the
 * space comes back.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/statfs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G103_ROOT	XFS_MNT "/g103"
#define G103_FILL	G103_ROOT "/spc"
#define G103_FILES	64
#define G103_VALSZ	65536
#define G103_LEAVE	(512 * 1024)	/* upstream leaves ~512k free */

static void g103_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_unlink(G103_FILL);
	for (i = 0; i < G103_FILES; i++) {
		snprintf(path, sizeof(path), G103_ROOT "/103.%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G103_ROOT);
}

static void xattrs_at_enospc_leave_the_files_removable(struct kunit *test)
{
	char path[64];
	struct kstatfs sfs;
	struct file *f;
	u64 avail, fill;
	u8 *value;
	int i, err, refused = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G103_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g103_remove_tree, NULL),
			0);

	value = kunit_kmalloc(test, G103_VALSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, value);
	memset(value, 'v', G103_VALSZ);

	for (i = 0; i < G103_FILES; i++) {
		snprintf(path, sizeof(path), G103_ROOT "/103.%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(path, "", 0), 0,
				    "creating %s failed", path);
	}

	/* _consume_freesp: fill all but ~512k */
	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &sfs), 0);
	avail = (u64)sfs.f_bavail * sfs.f_bsize;
	KUNIT_ASSERT_GT_MSG(test, avail, (u64)G103_LEAVE,
			    "only %llu bytes free to start with", avail);
	fill = avail - G103_LEAVE;

	f = filp_open(G103_FILL, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE,
		      0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	err = vfs_fallocate(f, 0, 0, fill);
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "allocating %llu bytes: %d", fill,
			    err);

	for (i = 0; i < G103_FILES; i++) {
		snprintf(path, sizeof(path), G103_ROOT "/103.%d", i);
		err = xfs_setxattr(path, "user.test", value, G103_VALSZ, 0);
		if (err == -ENOSPC || err == -EDQUOT) {
			refused++;
			continue;
		}
		KUNIT_ASSERT_EQ_MSG(test, err, 0,
				    "setting user.test on %s returned %d",
				    path, err);
	}
	kunit_info(test, "%d of %d SETXATTRs were refused for space\n",
		   refused, G103_FILES);

	/* the point of the test: the files still come off cleanly */
	for (i = 0; i < G103_FILES; i++) {
		snprintf(path, sizeof(path), G103_ROOT "/103.%d", i);
		KUNIT_EXPECT_EQ_MSG(test, xfs_unlink(path), 0,
				    "unlinking %s failed", path);
	}
	KUNIT_EXPECT_EQ(test, xfs_unlink(G103_FILL), 0);

	KUNIT_EXPECT_EQ_MSG(test, xfs_wait_for_free_bytes(fill), 0,
			    "the %llu bytes never came back", fill);
}

static int g103_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g103_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g103_cases[] = {
	KUNIT_CASE_SLOW(xattrs_at_enospc_leave_the_files_removable),
	{}
};

static struct kunit_suite g103_suite = {
	.name		= "xfstests/generic/103",
	.suite_init	= g103_suite_init,
	.suite_exit	= g103_suite_exit,
	.test_cases	= g103_cases,
};

kunit_test_suites(&g103_suite);

MODULE_DESCRIPTION("xfstests generic/103 over a loopback NFS mount");
MODULE_LICENSE("GPL");
