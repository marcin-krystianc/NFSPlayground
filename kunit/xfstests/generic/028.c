// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/028 over a loopback NFS mount: path resolution stays
 * correct while a concurrent rename races it.
 *
 * Upstream's src/t_getcwd forks: the parent spins getcwd(2) on a directory
 * while the child repeatedly creates and renames a file, and asserts the
 * parent's answer never changes -- in particular never collapses to "/",
 * which is what a specific dcache race (fixed by ede4ceb/f650080, after
 * being introduced by 232d2d6) once produced. d_path()/prepend_path() walk
 * the d_parent chain under a global seqlock (dcache.c's rename_lock) that
 * any rename anywhere in the mount bumps, so the race is exercised by any
 * concurrent RENAME, not specifically one inside the resolved directory.
 *
 * A worker kthread runs the create/rename/rename/... chain (upstream's
 * do_rename) while the test thread repeatedly resolves a directory's path
 * via kern_path()+d_path() -- the in-kernel analog of getcwd(2) -- and
 * requires the answer to be exactly right every time, matching upstream's
 * every-iteration check. An earlier version of this port interleaved the
 * two sequentially instead of racing them, which cannot exercise the
 * seqlock retry path the bug lived in regardless of correctness: the
 * worker kthread pattern (record-error-in-a-struct, since a worker cannot
 * use KUnit assertions -- see kunit-nfs-reference.md) mirrors 037/084.
 *
 * After the race, a second, upstream-independent check goes further:
 * renaming an *ancestor* of the resolved directory should also update
 * every path below it without touching those dentries -- the case where
 * d_path has to be walking a parent chain a RENAME actually updated.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/limits.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>

#include "xfstests_nfs_fixture.h"

#define G_ROOT		XFS_MNT "/g028"
#define G_DEEP		G_ROOT "/a/b/c/d"
#define G028_READS	2000	/* foreground resolutions racing the renamer */

static void g_remove_tree(void *unused)
{
	char buf[96];
	int i;

	/* the renamer's chain is densely numbered; the first gap ends it */
	xfs_unlink(G_DEEP "/t_getcwd_testfile");
	for (i = 1; i < 100000; i++) {
		snprintf(buf, sizeof(buf), G_DEEP "/t_getcwd_testfile%d", i);
		if (xfs_unlink(buf))
			break;
	}

	xfs_unlink(G_DEEP "/sib0");
	xfs_unlink(G_ROOT "/a/b/c2/d/sib0");
	xfs_rmdir(G_ROOT "/a/b/c2/d");
	xfs_rmdir(G_ROOT "/a/b/c2");
	xfs_rmdir(G_ROOT "/a/b/c/d");
	xfs_rmdir(G_ROOT "/a/b/c");
	xfs_rmdir(G_ROOT "/a/b");
	xfs_rmdir(G_ROOT "/a");
	xfs_rmdir(G_ROOT);
}

/* getcwd()'s analog: resolve a path and render it back out */
static void g_expect_path(struct kunit *test, const char *lookup,
			  const char *expected, const char *ctx)
{
	char *buf, *rendered;
	struct path p;
	int err;

	buf = kunit_kmalloc(test, PATH_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	err = kern_path(lookup, 0, &p);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: resolving %s failed (%d)", ctx,
			    lookup, err);
	rendered = d_path(&p, buf, PATH_MAX);
	path_put(&p);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rendered),
			       "%s: d_path failed (%ld)", ctx,
			       PTR_ERR(rendered));

	/* the degenerate answer the original bug produced */
	KUNIT_ASSERT_STRNEQ_MSG(test, rendered, "/",
				"%s: d_path collapsed to \"/\"", ctx);
	KUNIT_ASSERT_STREQ_MSG(test, rendered, expected,
			       "%s: d_path gave \"%s\"", ctx, rendered);
}

struct g028_racer {
	atomic_t		*stop;
	int			err;
	unsigned long		renames;
	char			last_name[96];
	struct completion	started;
	struct completion	done;
};

/*
 * upstream's do_rename(): create the file once, then rename it through an
 * incrementing chain (t_getcwd_testfile -> t_getcwd_testfile1 -> ...)
 * until told to stop. `started` is signalled once the initial create
 * lands, so the test thread can wait for the race to actually be
 * underway instead of assuming kthread_run() scheduled it in time (see
 * 037's fix for why that assumption is unsafe).
 */
static int g028_rename_chain(void *arg)
{
	struct g028_racer *r = arg;
	char cur[96], next[96];
	unsigned long i = 0;
	int err;

	strscpy(cur, G_DEEP "/t_getcwd_testfile", sizeof(cur));
	err = xfs_write_new_file(cur, "x", 1);
	if (err) {
		r->err = err;
		complete(&r->started);
		complete(&r->done);
		return 0;
	}
	strscpy(r->last_name, cur, sizeof(r->last_name));
	complete(&r->started);

	while (!atomic_read(r->stop)) {
		i++;
		snprintf(next, sizeof(next), G_DEEP "/t_getcwd_testfile%lu", i);
		err = xfs_rename(cur, next);
		if (err) {
			r->err = err;
			break;
		}
		strscpy(cur, next, sizeof(cur));
		strscpy(r->last_name, cur, sizeof(r->last_name));
		r->renames++;
		cond_resched();
	}
	complete(&r->done);
	return 0;
}

static void paths_stay_correct_while_the_tree_churns(struct kunit *test)
{
	struct g028_racer racer = { };
	struct task_struct *t;
	atomic_t stop = ATOMIC_INIT(0);
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g_remove_tree, NULL), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a/b"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT "/a/b/c"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_DEEP), 0);

	g_expect_path(test, G_DEEP, G_DEEP, "initial");

	/*
	 * The race: a worker renames a sibling file under G_DEEP in a tight
	 * loop while the test thread repeatedly resolves G_DEEP's own path,
	 * matching upstream's parent/child shape exactly.
	 */
	racer.stop = &stop;
	init_completion(&racer.started);
	init_completion(&racer.done);
	t = kthread_run(g028_rename_chain, &racer, "g028-rename");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld", PTR_ERR(t));

	KUNIT_ASSERT_NE_MSG(test,
			    wait_for_completion_timeout(&racer.started,
							msecs_to_jiffies(10000)),
			    0UL, "rename worker made no progress within 10s");
	KUNIT_ASSERT_EQ_MSG(test, racer.err, 0,
			    "creating the racer's file failed: %d", racer.err);

	for (i = 0; i < G028_READS; i++) {
		g_expect_path(test, G_DEEP, G_DEEP, "racing rename");
		cond_resched();
	}

	atomic_set(&stop, 1);
	wait_for_completion(&racer.done);
	KUNIT_EXPECT_EQ_MSG(test, racer.err, 0, "rename chain failed with %d",
			    racer.err);
	KUNIT_EXPECT_GT_MSG(test, racer.renames, 0UL,
			    "rename chain never completed a rename");
	xfs_unlink(racer.renames ? racer.last_name :
		   G_DEEP "/t_getcwd_testfile");

	/*
	 * The harder case, not in upstream: rename an ancestor. Every path
	 * below it changes without any of those dentries being touched, so
	 * d_path has to be walking a parent chain the RENAME actually
	 * updated.
	 */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G_DEEP "/sib0", "s", 1), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G_ROOT "/a/b/c", G_ROOT "/a/b/c2"), 0);

	g_expect_path(test, G_ROOT "/a/b/c2/d", G_ROOT "/a/b/c2/d",
		      "after ancestor rename");
	g_expect_path(test, G_ROOT "/a/b/c2/d/sib0", G_ROOT "/a/b/c2/d/sib0",
		      "child after ancestor rename");
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G_DEEP),
			       "the old ancestor path still resolves");

	KUNIT_ASSERT_EQ(test, xfs_unlink(G_ROOT "/a/b/c2/d/sib0"), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_rename(G_ROOT "/a/b/c2", G_ROOT "/a/b/c"), 0);
	g_expect_path(test, G_DEEP, G_DEEP, "after renaming back");
}

static int g_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g_cases[] = {
	KUNIT_CASE_SLOW(paths_stay_correct_while_the_tree_churns),
	{}
};

static struct kunit_suite g_suite = {
	.name		= "xfstests/generic/028",
	.suite_init	= g_suite_init,
	.suite_exit	= g_suite_exit,
	.test_cases	= g_cases,
};

kunit_test_suites(&g_suite);

MODULE_DESCRIPTION("xfstests generic/028 over a loopback NFS mount");
MODULE_LICENSE("GPL");
