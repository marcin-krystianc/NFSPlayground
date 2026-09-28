// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/604 over a loopback NFS mount: an unmount racing a
 * mount of the same filesystem.
 *
 * Upstream writes 501 4K files on SCRATCH_MNT, starts _scratch_unmount in
 * the background, sleeps 10 ms and runs _scratch_mount, hoping mount()
 * reaches s_umount after umount has taken it. The pass criterion is that
 * both commands succeed and nothing hangs.
 *
 * Over NFS the unmount is nfs_kill_super() writing back and releasing the
 * superblock, and the mount is nfs_get_tree() looking for a superblock to
 * share while that is under way.
 *
 * Deviations: SCRATCH_MNT is the fixture's second mount of the export
 * (XFS_SCRATCH_MNT, nosharecache), so every mount gets a new superblock
 * and the race is against sget_fc() creating one, not reusing the dying
 * one. The unmount runs in a kthread. The files go into a directory, not
 * the export's root, so the shared export stays clean.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G604_DIR	"/g604"
#define G604_FILES	501

struct g604_umount {
	int			err;
	struct completion	done;
};

static int g604_do_umount(void *arg)
{
	struct g604_umount *u = arg;

	u->err = xfs_umount(XFS_SCRATCH_MNT);
	complete(&u->done);
	return 0;
}

static void g604_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_scratch_umount();
	for (i = 0; i < G604_FILES; i++) {
		snprintf(path, sizeof(path), XFS_MNT G604_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(XFS_MNT G604_DIR);
}

static void umount_races_mount(struct kunit *test)
{
	struct g604_umount u = {};
	struct task_struct *t;
	char path[64];
	u8 *buf;
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g604_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_SCRATCH_MNT G604_DIR), 0);

	buf = kunit_kzalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	for (i = 0; i < G604_FILES; i++) {
		snprintf(path, sizeof(path), XFS_SCRATCH_MNT G604_DIR "/%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(path, buf, 4096),
				    0, "pwrite to %s", path);
	}
	/* the closes above left delayed fputs that would make umount EBUSY */
	xfs_settle_fput();

	init_completion(&u.done);
	t = kthread_run(g604_do_umount, &u, "g604-umount");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));
	msleep(10);
	err = xfs_scratch_mount();
	wait_for_completion(&u.done);

	KUNIT_EXPECT_EQ_MSG(test, u.err, 0, "umount: %d", u.err);
	KUNIT_EXPECT_EQ_MSG(test, err, 0, "mount: %d", err);

	/* the filesystem the mount produced is usable */
	snprintf(path, sizeof(path), XFS_SCRATCH_MNT G604_DIR "/%d",
		 G604_FILES - 1);
	KUNIT_EXPECT_TRUE(test, xfs_exists(path));
}

static int g604_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g604_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g604_cases[] = {
	KUNIT_CASE_SLOW(umount_races_mount),
	{}
};

static struct kunit_suite g604_suite = {
	.name		= "xfstests/generic/604",
	.suite_init	= g604_suite_init,
	.suite_exit	= g604_suite_exit,
	.test_cases	= g604_cases,
};

kunit_test_suites(&g604_suite);

MODULE_DESCRIPTION("xfstests generic/604 over a loopback NFS mount");
MODULE_LICENSE("GPL");
