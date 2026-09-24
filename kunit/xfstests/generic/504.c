// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/504 over a loopback NFS mount: /proc/locks lists a
 * flock whose owner has exited.
 *
 * Upstream opens a file on a shell fd, runs flock(1) -x on that fd (the
 * flock process takes the lock on the shared open file and exits), and
 * greps /proc/locks for ":<inode> ". The lock outlives the process because
 * a flock belongs to the open file, and lock_get_status() prints pid 0
 * for an owner that no longer exists.
 *
 * Over NFS the flock goes through nfs_flock(), which takes a whole-file
 * LOCK on the server and records the FL_FLOCK lock locally, which is what
 * /proc/locks reports.
 *
 * Deviations: the flock(1) process is a kthread that takes the lock on the
 * test's open file and exits. /proc is mounted by the port for the length
 * of the test, since the UML test kernel has no init to mount it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G504_ROOT	XFS_MNT "/g504"
#define G504_FILE	G504_ROOT "/flock_testfile_504"
#define G504_LOCKS_SZ	16384

struct g504_flocker {
	struct file		*f;
	int			err;
	struct completion	done;
};

/* flock(1) -x: sys_flock()'s body for LOCK_EX */
static int g504_flock(void *arg)
{
	struct g504_flocker *l = arg;
	struct file_lock *fl;

	fl = locks_alloc_lock();
	if (!fl) {
		l->err = -ENOMEM;
		goto out;
	}
	fl->c.flc_file = l->f;
	fl->c.flc_owner = l->f;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_flags = FL_FLOCK | FL_SLEEP;
	fl->c.flc_type = F_WRLCK;
	fl->fl_end = OFFSET_MAX;
	if (l->f->f_op->flock)
		l->err = l->f->f_op->flock(l->f, F_SETLKW, fl);
	else
		l->err = locks_lock_file_wait(l->f, fl);
	locks_free_lock(fl);
out:
	complete(&l->done);
	return 0;
}

static void g504_remove_tree(void *unused)
{
	xfs_umount("/proc");
	xfs_unlink(G504_FILE);
	xfs_rmdir_settled(G504_ROOT);
}

static void proc_locks_lists_a_dead_owners_flock(struct kunit *test)
{
	struct g504_flocker l = {};
	struct task_struct *t;
	struct file *pl;
	struct kstat st;
	char needle[32];
	loff_t pos = 0;
	ssize_t n, len = 0;
	char *buf;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G504_ROOT), 0);
	err = xfs_mkdir("/proc");
	KUNIT_ASSERT_TRUE_MSG(test, !err || err == -EEXIST, "mkdir /proc: %d",
			      err);
	KUNIT_ASSERT_EQ(test, xfs_mount_at("proc", "/proc", "proc", NULL), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g504_remove_tree, NULL),
			0);

	/* touch $testfile; exec {test_fd}> $testfile */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G504_FILE, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G504_FILE, &st), 0);
	l.f = filp_open(G504_FILE, O_WRONLY | O_TRUNC, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(l.f), "open: %ld", PTR_ERR(l.f));

	/* flock -x $test_fd, from a task that then exits */
	init_completion(&l.done);
	t = kthread_run(g504_flock, &l, "g504-flock");
	if (IS_ERR(t)) {
		filp_close(l.f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "kthread_run: %ld", PTR_ERR(t));
	}
	wait_for_completion(&l.done);
	if (l.err) {
		filp_close(l.f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "flock: %d", l.err);
	}

	/* cat /proc/locks */
	buf = kunit_kzalloc(test, G504_LOCKS_SZ, GFP_KERNEL);
	pl = filp_open("/proc/locks", O_RDONLY, 0);
	if (!buf || IS_ERR(pl)) {
		filp_close(l.f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "open /proc/locks: %ld",
				     buf ? PTR_ERR(pl) : -ENOMEM);
	}
	while (len < G504_LOCKS_SZ - 1 &&
	       (n = kernel_read(pl, buf + len, G504_LOCKS_SZ - 1 - len,
				&pos)) > 0)
		len += n;
	filp_close(pl, NULL);
	filp_close(l.f, NULL);

	/* grep -q ":$tf_inode " /proc/locks */
	snprintf(needle, sizeof(needle), ":%llu ", st.ino);
	KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(buf, needle),
				  "lock info not found for inode %llu in:\n%s",
				  st.ino, buf);
}

static int g504_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g504_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g504_cases[] = {
	KUNIT_CASE(proc_locks_lists_a_dead_owners_flock),
	{}
};

static struct kunit_suite g504_suite = {
	.name		= "xfstests/generic/504",
	.suite_init	= g504_suite_init,
	.suite_exit	= g504_suite_exit,
	.test_cases	= g504_cases,
};

kunit_test_suites(&g504_suite);

MODULE_DESCRIPTION("xfstests generic/504 over a loopback NFS mount");
MODULE_LICENSE("GPL");
