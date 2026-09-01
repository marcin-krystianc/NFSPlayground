// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/102 over a loopback NFS mount: write/delete ENOSPC loop.
 *
 * Upstream loops writing and deleting on a small filesystem: freed space
 * must become reusable, cycle after cycle. Over NFS "immediately" is not
 * part of the contract -- knfsd's file cache can hold a removed file's
 * space for a moment -- so each cycle waits (bounded) for the space to
 * return before writing again. Over NFS this is
 * WRITE/COMMIT pressure against server-side ENOSPC plus REMOVE, with the
 * client's dirty cache in between -- a lost "space freed" transition
 * makes a later cycle fail.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G102_ROOT	XFS_MNT "/g102"

/* write stamped 64K chunks + fsync until the server says ENOSPC */
static int g102_fill(struct kunit *test, const char *path, loff_t *written)
{
	struct file *f;
	u8 *buf;
	loff_t pos = 0;
	int err = 0, i;

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	for (i = 0; i < 1024; i++) {	/* 64M ceiling >> the 16M export */
		ssize_t n;

		memset(buf, 0x41 + (i % 26), 65536);
		n = kernel_write(f, buf, 65536, &pos);
		if (n < 0) {
			err = n;
			break;
		}
		err = vfs_fsync(f, 0);
		if (err)
			break;
	}
	filp_close(f, NULL);
	*written = pos;
	return err;
}

static void g102_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < 4; i++) {
		snprintf(buf, sizeof(buf), G102_ROOT "/f%d", i);
		xfs_unlink(buf);
	}
	xfs_unlink(G102_ROOT "/fill");
	xfs_rmdir_settled(G102_ROOT);
}

static void freed_space_is_reusable_every_cycle(struct kunit *test)
{
	char name[64];
	u8 *buf;
	int cycle, i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G102_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g102_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x77, 65536);

	for (cycle = 0; cycle < 6; cycle++) {
		/* 4 x 2MB = 8MB per cycle on a 16MB export */
		for (i = 0; i < 4; i++) {
			struct file *f;
			loff_t pos = 0;
			int blk, err = 0;

			snprintf(name, sizeof(name), G102_ROOT "/f%d", i);
			f = filp_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
					       "cycle %d: creating %s: %ld",
					       cycle, name, PTR_ERR(f));
			/*
			 * No assertions while the file is open: an aborted
			 * case leaks the struct file and wedges the mount
			 * for every suite after this one.
			 */
			for (blk = 0; blk < 32 && !err; blk++) {
				ssize_t n = kernel_write(f, buf, 65536, &pos);

				if (n != 65536)
					err = (n < 0) ? n : -EIO;
			}
			if (!err)
				err = vfs_fsync(f, 0);
			filp_close(f, NULL);
			KUNIT_ASSERT_EQ_MSG(test, err, 0,
					    "cycle %d file %d: write/fsync %d"
					    " -- freed space not reusable?",
					    cycle, i, err);
		}
		for (i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), G102_ROOT "/f%d", i);
			KUNIT_ASSERT_EQ(test, xfs_unlink(name), 0);
		}
		/*
		 * Space from REMOVEd files returns when the server drops its
		 * file-cache references -- eventually-true, so wait for at
		 * least 10MB available before the next 8MB cycle.
		 */
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_wait_for_free_bytes(10 * 1024 * 1024),
				    0, "cycle %d: freed space never came back",
				    cycle);
	}
}

static int g102_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=8192");
	return xfstests_nfs_get();
}

static void g102_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g102_cases[] = {
	KUNIT_CASE_SLOW(freed_space_is_reusable_every_cycle),
	{}
};

static struct kunit_suite g102_suite = {
	.name		= "xfstests/generic/102",
	.suite_init	= g102_suite_init,
	.suite_exit	= g102_suite_exit,
	.test_cases	= g102_cases,
};

kunit_test_suites(&g102_suite);

MODULE_DESCRIPTION("xfstests generic/102 over a loopback NFS mount");
MODULE_LICENSE("GPL");
