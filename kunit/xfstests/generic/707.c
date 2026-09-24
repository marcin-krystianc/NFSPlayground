// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/707 over a loopback NFS mount: moving a directory
 * while its contents are changing.
 *
 * Upstream creates a chain of parent directories, then repeatedly: makes
 * a directory, starts a background process filling it with files, and
 * moves that directory through every parent in the chain while the files
 * are still being created. Directory formats change as a directory grows
 * (udf and ext4 both had corruption bugs here -- f950fd052913 and
 * 0813299c586b); a rename that runs while the format is being converted
 * is what the test is for.
 *
 * Over NFS the rename is a RENAME RPC that changes the directory's
 * parent while the client still holds dentries and a readdir cache for
 * it, and the creating side is issuing CREATEs against a directory whose
 * path is moving underneath it. Some of those CREATEs are expected to
 * fail with ENOENT -- upstream's creator works from a cwd that follows
 * the directory, which a kernel test cannot do without sharing its
 * fs_struct -- so the port counts what it managed to create and requires
 * every one of those names to be present afterwards.
 *
 * Deviations: 20 parents and 60 files rather than 500 and 500, a few
 * loops rather than a hundred; the creator is a kthread using absolute
 * paths.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>

#include "xfstests_nfs_fixture.h"

#define G707_ROOT	XFS_MNT "/g707"
#define G707_MOVES	20
#define G707_FILES	60
#define G707_LOOPS	3

struct g707_creator {
	int			move;		/* which parent holds it now */
	atomic_t		*where;
	int			created;
	int			err;
	struct completion	done;
};

/* the directory's current path is $ROOT/dir<n>/dir */
static void g707_dirpath(char *buf, size_t size, int n)
{
	snprintf(buf, size, G707_ROOT "/dir%d/dir", n);
}

static int g707_create_files(void *arg)
{
	struct g707_creator *c = arg;
	char path[96];
	int i;

	for (i = 0; i < G707_FILES; i++) {
		int at = atomic_read(c->where);
		int err;

		snprintf(path, sizeof(path),
			 G707_ROOT "/dir%d/dir/somewhatlongerfilename%d", at,
			 i);
		err = xfs_write_new_file(path, "", 0);
		if (!err)
			c->created++;
		else if (err != -ENOENT && err != -ESTALE)
			c->err = err;
		cond_resched();
	}
	complete(&c->done);
	return 0;
}

static void g707_remove_tree(void *unused)
{
	char path[96];
	int i, j;

	for (i = 0; i <= G707_MOVES; i++) {
		for (j = 0; j < G707_FILES; j++) {
			snprintf(path, sizeof(path),
				 G707_ROOT "/dir%d/dir/somewhatlongerfilename%d",
				 i, j);
			xfs_unlink(path);
		}
		g707_dirpath(path, sizeof(path), i);
		xfs_rmdir_settled(path);
		snprintf(path, sizeof(path), G707_ROOT "/dir%d", i);
		xfs_rmdir_settled(path);
	}
	xfs_rmdir_settled(G707_ROOT);
}

static void moving_a_directory_while_it_grows(struct kunit *test)
{
	atomic_t where = ATOMIC_INIT(0);
	struct task_struct *t;
	char from[96], to[96], path[96];
	int loop, i, j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G707_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g707_remove_tree, NULL),
			0);

	for (i = 0; i <= G707_MOVES; i++) {
		snprintf(path, sizeof(path), G707_ROOT "/dir%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(path), 0,
				    "creating %s failed", path);
	}

	for (loop = 0; loop < G707_LOOPS; loop++) {
		struct g707_creator c = { .where = &where };

		atomic_set(&where, 0);
		g707_dirpath(path, sizeof(path), 0);
		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(path), 0,
				    "loop %d: creating the moving directory failed",
				    loop);

		init_completion(&c.done);
		t = kthread_run(g707_create_files, &c, "g707-creator");
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));

		for (i = 0; i < G707_MOVES; i++) {
			g707_dirpath(from, sizeof(from), i);
			g707_dirpath(to, sizeof(to), i + 1);
			KUNIT_ASSERT_EQ_MSG(test, xfs_rename(from, to), 0,
					    "loop %d: moving %s to %s failed",
					    loop, from, to);
			atomic_set(&where, i + 1);
			cond_resched();
		}

		wait_for_completion(&c.done);
		KUNIT_EXPECT_EQ_MSG(test, c.err, 0,
				    "loop %d: the creator failed with %d",
				    loop, c.err);

		/* everything it did create is in the directory's new home */
		for (j = 0, i = 0; i < G707_FILES && j < c.created; i++) {
			snprintf(path, sizeof(path),
				 G707_ROOT "/dir%d/dir/somewhatlongerfilename%d",
				 G707_MOVES, i);
			if (xfs_exists(path))
				j++;
		}
		KUNIT_EXPECT_EQ_MSG(test, j, c.created,
				    "loop %d: %d of the %d created files are missing after the moves",
				    loop, c.created - j, c.created);

		/* clean the directory out before the next loop */
		for (i = 0; i < G707_FILES; i++) {
			snprintf(path, sizeof(path),
				 G707_ROOT "/dir%d/dir/somewhatlongerfilename%d",
				 G707_MOVES, i);
			xfs_unlink(path);
		}
		g707_dirpath(path, sizeof(path), G707_MOVES);
		KUNIT_ASSERT_EQ_MSG(test, xfs_rmdir_settled(path), 0,
				    "loop %d: the moved directory would not rmdir",
				    loop);
	}
}

static int g707_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g707_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g707_cases[] = {
	KUNIT_CASE_SLOW(moving_a_directory_while_it_grows),
	{}
};

static struct kunit_suite g707_suite = {
	.name		= "xfstests/generic/707",
	.suite_init	= g707_suite_init,
	.suite_exit	= g707_suite_exit,
	.test_cases	= g707_cases,
};

kunit_test_suites(&g707_suite);

MODULE_DESCRIPTION("xfstests generic/707 over a loopback NFS mount");
MODULE_LICENSE("GPL");
