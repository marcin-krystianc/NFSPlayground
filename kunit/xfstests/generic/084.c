// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/084 over a loopback NFS mount: a link/unlink storm
 * against a name that keeps being deleted and recreated, while
 * open-but-unlinked files exist.
 *
 * Upstream starts src/multi_open_unlink to keep a set of unlinked files
 * open (so the filesystem's unlinked-inode list is not empty), then runs
 * one link/unlink loop per CPU against a target file while a third loop
 * deletes and recreates that target underneath them. It is the
 * regression test for commit aae8a97 ("fs: Don't allow to create
 * hardlink for deleted file"); the pass criterion is that nothing oopses
 * or hangs.
 *
 * Over NFS "open but unlinked" means sillyrename: the client renames the
 * file to .nfsXXXX and removes it on last close, so the storm runs
 * against a directory that also contains silly-renamed entries. The
 * LINKs race REMOVEs of their own target, which is precisely the case
 * where the client can be holding a dentry for a name the server no
 * longer has.
 *
 * Deviations: two kthreads rather than one per CPU, bounded rounds
 * rather than "until killed", and the expected races are named: a LINK
 * may fail with ENOENT (the source was just removed) and a REMOVE with
 * ENOENT (someone else got there first). Anything else is recorded and
 * asserted on by the test thread. The port also checks afterwards that
 * the directory can be emptied, which upstream leaves to its fsck.
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

#define G084_ROOT	XFS_MNT "/g084"
#define G084_SRC	G084_ROOT "/084.target"
#define G084_OPEN	4		/* open-unlinked files */
#define G084_STORMS	2
#define G084_ROUNDS	200

struct g084_storm {
	int			id;
	atomic_t		*stop;
	int			err;
	unsigned long		links;
	struct completion	done;
};

static int g084_link_unlink(void *arg)
{
	struct g084_storm *s = arg;
	char target[64];
	int i = 0;

	while (!atomic_read(s->stop)) {
		int err;

		snprintf(target, sizeof(target), G084_ROOT "/084.link.%d.%d",
			 s->id, i++ & 7);
		err = xfs_link(G084_SRC, target);
		if (err && err != -ENOENT && err != -EEXIST) {
			s->err = err;
			break;
		}
		if (!err)
			s->links++;
		err = xfs_unlink(target);
		if (err && err != -ENOENT) {
			s->err = err;
			break;
		}
		cond_resched();
	}
	complete(&s->done);
	return 0;
}

static void g084_remove_tree(void *unused)
{
	char path[64];
	int i, j;

	for (i = 0; i < G084_STORMS; i++)
		for (j = 0; j < 8; j++) {
			snprintf(path, sizeof(path),
				 G084_ROOT "/084.link.%d.%d", i, j);
			xfs_unlink(path);
		}
	for (i = 0; i < G084_OPEN; i++) {
		snprintf(path, sizeof(path), G084_ROOT "/084.unlinked.%d", i);
		xfs_unlink(path);
	}
	xfs_unlink(G084_SRC);
	xfs_rmdir_settled(G084_ROOT);
}

static void a_link_storm_against_a_vanishing_target(struct kunit *test)
{
	struct file *open_unlinked[G084_OPEN];
	struct g084_storm storms[G084_STORMS];
	struct task_struct *t;
	atomic_t stop = ATOMIC_INIT(0);
	char path[64];
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G084_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g084_remove_tree, NULL),
			0);

	/* multi_open_unlink: files that are open and already unlinked */
	for (i = 0; i < G084_OPEN; i++) {
		snprintf(path, sizeof(path), G084_ROOT "/084.unlinked.%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "x", 1), 0);
		open_unlinked[i] = filp_open(path, O_RDWR, 0);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(open_unlinked[i]),
				       "open %d: %ld", i,
				       PTR_ERR(open_unlinked[i]));
		KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(path), 0,
				    "unlinking the open file %d failed", i);
	}

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G084_SRC, "t", 1), 0);

	for (i = 0; i < G084_STORMS; i++) {
		storms[i] = (struct g084_storm){ .id = i, .stop = &stop };
		init_completion(&storms[i].done);
		t = kthread_run(g084_link_unlink, &storms[i], "g084-storm%d",
				i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
	}

	/* the third loop: remove and recreate the link target */
	for (i = 0; i < G084_ROUNDS; i++) {
		err = xfs_unlink(G084_SRC);
		KUNIT_ASSERT_TRUE_MSG(test, err == 0 || err == -ENOENT,
				      "round %d: unlink returned %d", i, err);
		err = xfs_write_new_file(G084_SRC, "t", 1);
		KUNIT_ASSERT_EQ_MSG(test, err, 0,
				    "round %d: recreating the target failed: %d",
				    i, err);
		cond_resched();
	}

	atomic_set(&stop, 1);
	for (i = 0; i < G084_STORMS; i++) {
		wait_for_completion(&storms[i].done);
		KUNIT_EXPECT_EQ_MSG(test, storms[i].err, 0,
				    "storm %d failed with %d", i,
				    storms[i].err);
		KUNIT_EXPECT_GT_MSG(test, storms[i].links, 0UL,
				    "storm %d never managed a link", i);
	}

	for (i = 0; i < G084_OPEN; i++)
		filp_close(open_unlinked[i], NULL);
}

static int g084_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g084_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g084_cases[] = {
	KUNIT_CASE_SLOW(a_link_storm_against_a_vanishing_target),
	{}
};

static struct kunit_suite g084_suite = {
	.name		= "xfstests/generic/084",
	.suite_init	= g084_suite_init,
	.suite_exit	= g084_suite_exit,
	.test_cases	= g084_cases,
};

kunit_test_suites(&g084_suite);

MODULE_DESCRIPTION("xfstests generic/084 over a loopback NFS mount");
MODULE_LICENSE("GPL");
