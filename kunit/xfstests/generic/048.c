// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/048 over a loopback NFS mount: whole-filesystem sync
 * durability. **generic/049 is folded in here as a second case** -- see
 * below.
 *
 * 048 and 049 are the sync members of the 043-049 "NULL files problem"
 * family, the same shape as generic/047 but reaching the server through
 * syncfs rather than a per-file fsync. Both shut the filesystem down with
 * the XFS shutdown ioctl and check after remount that no file came back the
 * right length full of NULs.
 *
 * The shutdown has no NFS equivalent and the "no extents" test is a fiemap
 * question NFSv4.2 cannot answer, so as in 047 what ports is the durability
 * half: after a whole-filesystem sync, every file must be complete on the
 * server. Here that means sync_filesystem() has to have driven the client's
 * dirty pages out through WRITE and COMMIT for every inode on the mount --
 * a different path from 047's per-file nfs_file_fsync(), and the only place
 * in the ported set where the whole superblock is synced as the durability
 * mechanism.
 *
 * **On folding 049 in.** 048 writes each file and syncs; 049 writes every
 * file with no sync at all and then syncs once at the end. Upstream keeps
 * them apart because the XFS log replay paths they expose after a shutdown
 * differ. Without a shutdown there is no such divergence and the two
 * collapse onto the same NFS code path, so a separate 049 suite would be a
 * copy of this file with one loop moved. It is a case here instead, and the
 * docs list 049 as folded rather than as a port in its own right -- the
 * distinction upstream draws is real, and this deployment cannot see it.
 *
 * Deviations: upstream's 999 files become G048_FILES and its 10 MB files
 * become G048_SIZE -- 048 requires 10 GB of scratch space, and the fixture's
 * export is 64 MB. Neither number is what the test is about. The 0xff fill
 * is upstream's.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>

#include "xfstests_nfs_fixture.h"

#define G048_ROOT	XFS_MNT "/g048"
#define G048_EXPORT	XFS_EXPORT "/g048"

#define G048_FILES	120		/* upstream's 999, scaled */
#define G048_SIZE	(32 * 1024)	/* upstream's 10 MB will not fit */
#define G048_FILL	0xff

static void g048_remove_tree(void *unused)
{
	char name[80];
	int i;

	for (i = 1; i <= G048_FILES; i++) {
		snprintf(name, sizeof(name), G048_ROOT "/%d", i);
		xfs_unlink(name);
	}
	xfs_rmdir_settled(G048_ROOT);
}

/*
 * syncfs(2) on the NFS mount. The s_umount read lock is required:
 * sync_filesystem() opens with WARN_ON(!rwsem_is_locked(&sb->s_umount))
 * (fs/sync.c:38), as does sync_inodes_sb() in two places. This mirrors
 * SYSCALL_DEFINE1(syncfs).
 */
static int g048_syncfs(void)
{
	struct super_block *sb;
	struct path root;
	int err;

	err = kern_path(XFS_MNT, 0, &root);
	if (err)
		return err;

	sb = root.mnt->mnt_sb;
	down_read(&sb->s_umount);
	err = sync_filesystem(sb);
	up_read(&sb->s_umount);

	path_put(&root);
	return err;
}

/* write one file; no assertions taken while the struct file is open */
static int g048_write_one(const char *path, const u8 *buf)
{
	struct file *f;
	loff_t off;
	int err = 0;

	f = filp_open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (IS_ERR(f))
		return PTR_ERR(f);

	for (off = 0; off < G048_SIZE && !err; off += PAGE_SIZE) {
		loff_t pos = off;
		ssize_t n = kernel_write(f, buf, PAGE_SIZE, &pos);

		if (n != PAGE_SIZE)
			err = (n < 0) ? (int)n : -EIO;
	}
	filp_close(f, NULL);
	return err;
}

static void g048_check_all(struct kunit *test, u8 *got, const char *how)
{
	char name[80];
	int i;

	for (i = 1; i <= G048_FILES; i++) {
		struct kstat st;
		loff_t off;

		snprintf(name, sizeof(name), G048_ROOT "/%d", i);
		KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(name),
				      "%s: file %s missing - sync failed", how,
				      name);
		KUNIT_ASSERT_EQ(test, xfs_kstat(name, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)G048_SIZE,
				    "%s: file %s has incorrect size %lld - sync failed",
				    how, name, st.size);

		snprintf(name, sizeof(name), G048_EXPORT "/%d", i);
		for (off = 0; off < G048_SIZE; off += PAGE_SIZE) {
			ssize_t r = xfs_read_range(name, got, PAGE_SIZE, off);
			size_t j;

			KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)PAGE_SIZE,
					    "%s: server read of %s at %lld returned %zd",
					    how, name, off, r);
			for (j = 0; j < PAGE_SIZE; j++)
				if (got[j] != G048_FILL) {
					KUNIT_FAIL(test,
						   "%s: corrupt file %s - byte %lld is %02x, expected %02x (sync did not reach the server)",
						   how, name,
						   off + (loff_t)j, got[j],
						   G048_FILL);
					return;
				}
		}
	}
}

static void g048_run(struct kunit *test, bool sync_each, const char *how)
{
	char name[80];
	u8 *buf, *got;
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G048_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g048_remove_tree,
						  NULL), 0);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	memset(buf, G048_FILL, PAGE_SIZE);

	for (i = 1; i <= G048_FILES; i++) {
		snprintf(name, sizeof(name), G048_ROOT "/%d", i);
		err = g048_write_one(name, buf);
		KUNIT_ASSERT_EQ_MSG(test, err, 0,
				    "%s: error creating/writing file %s: %d",
				    how, name, err);
		if (sync_each) {
			err = g048_syncfs();
			KUNIT_ASSERT_EQ_MSG(test, err, 0,
					    "%s: syncfs after %s: %d", how, name,
					    err);
		}
	}

	if (!sync_each) {
		err = g048_syncfs();
		KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: final syncfs: %d", how,
				    err);
	}

	g048_check_all(test, got, how);
}

/* generic/048: write each file, sync as you go */
static void syncfs_after_each_file_reaches_the_server(struct kunit *test)
{
	g048_run(test, true, "048 sync-per-file");
}

/* generic/049: write everything unsynced, then one sync at the end */
static void one_final_syncfs_reaches_the_server(struct kunit *test)
{
	g048_run(test, false, "049 single final sync");
}

static int g048_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g048_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g048_cases[] = {
	KUNIT_CASE_SLOW(syncfs_after_each_file_reaches_the_server),
	KUNIT_CASE_SLOW(one_final_syncfs_reaches_the_server),
	{}
};

static struct kunit_suite g048_suite = {
	.name		= "xfstests/generic/048",
	.suite_init	= g048_suite_init,
	.suite_exit	= g048_suite_exit,
	.test_cases	= g048_cases,
};

kunit_test_suites(&g048_suite);

MODULE_DESCRIPTION("xfstests generic/048 (and 049) over a loopback NFS mount");
MODULE_LICENSE("GPL");
