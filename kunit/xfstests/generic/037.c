// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/037 over a loopback NFS mount: xattr replace atomicity.
 *
 * Upstream races a background setxattr loop flipping one attribute between
 * two values against 1000 foreground getxattr reads, and requires every
 * read to land on one of the two values in full -- never a torn, partial,
 * or momentarily-missing read during the replace. This is the regression
 * test for "Btrfs: make xattr replace operations atomic", where a naive
 * replace was remove-then-insert rather than one operation.
 *
 * A worker kthread runs the flip loop while the test thread runs the 1000
 * reads concurrently, so the race window upstream depends on actually
 * exists here -- an earlier, sequential version of this port (setxattr
 * immediately followed by getxattr, nothing else running) could not
 * observe a torn read regardless of whether the replace was atomic. A
 * worker kthread cannot use KUnit assertions (see kunit-nfs-reference.md),
 * so it records its first error in a struct and the test thread asserts
 * on it after joining.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>

#include "xfstests_nfs_fixture.h"

#define G037_ROOT	XFS_MNT "/g037"
#define G037_FILE	G037_ROOT "/flip"
#define G037_READS	1000	/* foreground reads, as upstream */

static const char * const g037_vals[2] = { "foobar", "rabbit_hole" };

struct g037_flipper {
	atomic_t		*stop;
	int			err;
	unsigned long		flips;
	struct completion	started;	/* first iteration attempted */
	struct completion	done;
};

/*
 * kthread_run() only queues the new task; nothing guarantees it is
 * scheduled before the test thread's read loop finishes, and a fast
 * enough loopback run can complete all G037_READS iterations before this
 * thread gets a single timeslice. Signalling `started` after the first
 * attempt -- win or lose -- lets the test thread block until the race is
 * actually underway instead of assuming it started in time.
 */
static int g037_flip(void *arg)
{
	struct g037_flipper *f = arg;
	int i = 0;
	bool announced = false;

	while (!atomic_read(f->stop)) {
		const char *val = g037_vals[i++ & 1];
		int err = xfs_setxattr(G037_FILE, "user.something", val,
					strlen(val), 0);

		if (err) {
			f->err = err;
			break;
		}
		f->flips++;
		if (!announced) {
			complete(&f->started);
			announced = true;
		}
		cond_resched();
	}
	if (!announced)
		complete(&f->started);
	complete(&f->done);
	return 0;
}

static void g037_remove_tree(void *unused)
{
	xfs_unlink(G037_FILE);
	xfs_rmdir(G037_ROOT);
}

static void flipping_values_are_never_torn(struct kunit *test)
{
	struct g037_flipper flipper = { };
	struct task_struct *t;
	atomic_t stop = ATOMIC_INIT(0);
	char rd[32];
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G037_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g037_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G037_FILE, "f", 1), 0);

	err = xfs_setxattr(G037_FILE, "user.something", g037_vals[0],
			   strlen(g037_vals[0]), 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);

	flipper.stop = &stop;
	init_completion(&flipper.started);
	init_completion(&flipper.done);
	t = kthread_run(g037_flip, &flipper, "g037-flip");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld", PTR_ERR(t));

	KUNIT_ASSERT_NE_MSG(test,
			    wait_for_completion_timeout(&flipper.started,
							msecs_to_jiffies(10000)),
			    0UL, "flip worker made no progress within 10s");

	for (i = 0; i < G037_READS; i++) {
		ssize_t n = xfs_getxattr(G037_FILE, "user.something", rd,
					 sizeof(rd));

		KUNIT_ASSERT_GE_MSG(test, n, 0,
				    "read %d: getxattr failed: %zd (torn -- "
				    "attribute momentarily missing)", i, n);
		KUNIT_ASSERT_TRUE_MSG(test,
				      (n == (ssize_t)strlen(g037_vals[0]) &&
				       !memcmp(rd, g037_vals[0], n)) ||
				      (n == (ssize_t)strlen(g037_vals[1]) &&
				       !memcmp(rd, g037_vals[1], n)),
				      "read %d: torn value, length %zd", i, n);
		cond_resched();
	}

	atomic_set(&stop, 1);
	wait_for_completion(&flipper.done);
	KUNIT_EXPECT_EQ_MSG(test, flipper.err, 0, "flip loop failed with %d",
			    flipper.err);
	KUNIT_EXPECT_GT_MSG(test, flipper.flips, 0UL,
			    "flip loop never completed a flip");
}

static int g037_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g037_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g037_cases[] = {
	KUNIT_CASE_SLOW(flipping_values_are_never_torn),
	{}
};

static struct kunit_suite g037_suite = {
	.name		= "xfstests/generic/037",
	.suite_init	= g037_suite_init,
	.suite_exit	= g037_suite_exit,
	.test_cases	= g037_cases,
};

kunit_test_suites(&g037_suite);

MODULE_DESCRIPTION("xfstests generic/037 over a loopback NFS mount");
MODULE_LICENSE("GPL");
