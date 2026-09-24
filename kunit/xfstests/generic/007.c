// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/007 over a loopback NFS mount: nametest.
 *
 * src/nametest.c is a model-based directory consistency checker: a table
 * of 100 filenames each tracked as {exists, inode number}, hammered with
 * seeded random create/remove/lookup transactions. Every operation's
 * outcome is checked against the model: O_EXCL create must succeed iff
 * the model says the name is free (and the inode number is recorded),
 * unlink must succeed iff it exists, and stat must agree on both
 * existence and inode number. With -z it then removes every name the
 * model says is left.
 *
 * Over NFS this pits the client's dcache (positive and negative entries)
 * and inode-number handling (fileids from GETATTR) against the server's
 * truth across 100,000 CREATE/REMOVE/LOOKUP RPCs. A stale negative
 * dentry, a mis-cached fileid, or a lost REMOVE shows up as a model
 * mismatch.
 *
 * The run is upstream's exactly: seed 1, 100,000 iterations, and the
 * transaction mix switching every 100 iterations through upstream's three
 * zones (remove/create 20/60, 33/33, 60/20; the rest lookups). nametest
 * draws from xfstests' lib/random.c, which xfs_random() reproduces, so the
 * port replays upstream's sequence and its totals must equal the ones in
 * generic/007.out.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G007_ROOT	XFS_MNT "/g007"
#define G007_NAMES	100	/* nametest.1 .. nametest.100 */
#define G007_ITERS	100000
#define G007_SEED	1

static struct {
	bool	exists;
	u64	ino;
} g007_tab[G007_NAMES];

/* nametest's good_/bad_ counters */
static struct {
	int good_adds, bad_adds, good_rms, bad_rms, good_looks, bad_looks;
} g007_n;

static const char *g007_name(char *buf, int i)
{
	snprintf(buf, 64, G007_ROOT "/nametest.%d", i + 1);
	return buf;
}

static void g007_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G007_NAMES; i++)
		xfs_unlink(g007_name(buf, i));
	xfs_rmdir(G007_ROOT);
}

/* auto_create(): O_EXCL create, checked against the model. */
static void g007_create(struct kunit *test, int i)
{
	char buf[64];
	struct kstat st;
	struct file *f;

	f = filp_open(g007_name(buf, i), O_RDWR | O_CREAT | O_EXCL, 0666);
	if (!IS_ERR(f)) {
		filp_close(f, NULL);
		g007_n.good_adds++;
		KUNIT_ASSERT_EQ(test, xfs_kstat(buf, &st), 0);
		KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
				       "\"%s\"(%llu) created, but already existed as inumber %llu",
				       buf, st.ino, g007_tab[i].ino);
		g007_tab[i].exists = true;
		g007_tab[i].ino = st.ino;
		return;
	}
	KUNIT_ASSERT_EQ_MSG(test, PTR_ERR(f), (long)-EEXIST,
			    "create \"%s\": unexpected error %ld", buf,
			    PTR_ERR(f));
	g007_n.bad_adds++;
	KUNIT_ASSERT_TRUE_MSG(test, g007_tab[i].exists,
			      "\"%s\" not created, should not exist", buf);
}

/* auto_remove(): unlink, checked against the model. */
static void g007_remove(struct kunit *test, int i)
{
	char buf[64];
	int err = xfs_unlink(g007_name(buf, i));

	if (!err) {
		g007_n.good_rms++;
		KUNIT_ASSERT_TRUE_MSG(test, g007_tab[i].exists,
				      "\"%s\" removed, should not have existed",
				      buf);
		g007_tab[i].exists = false;
		g007_tab[i].ino = 0;
		return;
	}
	KUNIT_ASSERT_EQ_MSG(test, err, -ENOENT,
			    "remove \"%s\": unexpected error %d", buf, err);
	g007_n.bad_rms++;
	KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
			       "\"%s\"(%llu) not removed, should have existed",
			       buf, g007_tab[i].ino);
}

/* auto_lookup(): stat, checked for existence and inode number. */
static void g007_lookup(struct kunit *test, int i)
{
	char buf[64];
	struct kstat st;
	int err = xfs_kstat(g007_name(buf, i), &st);

	if (!err) {
		g007_n.good_looks++;
		KUNIT_ASSERT_TRUE_MSG(test, g007_tab[i].exists,
				      "\"%s\"(%llu) lookup, should not exist",
				      buf, st.ino);
		KUNIT_ASSERT_EQ_MSG(test, st.ino, g007_tab[i].ino,
				    "\"%s\"(%llu) lookup, should be inumber %llu",
				    buf, st.ino, g007_tab[i].ino);
		return;
	}
	KUNIT_ASSERT_EQ_MSG(test, err, -ENOENT,
			    "lookup \"%s\": unexpected error %d", buf, err);
	g007_n.bad_looks++;
	KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
			       "\"%s\"(%llu) lookup, should exist", buf,
			       g007_tab[i].ino);
}

static void nametest_matches_the_model_and_the_golden_counts(struct kunit *test)
{
	struct xfs_random rnd;
	int pct_remove = 0, pct_create = 0, zone = -1;
	int iter, i, op, cleanup = 0;
	char buf[64];

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G007_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g007_remove_tree,
						  NULL), 0);

	memset(g007_tab, 0, sizeof(g007_tab));
	memset(&g007_n, 0, sizeof(g007_n));
	xfs_srandom(&rnd, G007_SEED);
	for (iter = 0; iter < G007_ITERS; iter++) {
		/* the distribution of transaction types changes over time */
		if ((iter % G007_NAMES) == 0) {
			zone++;
			switch (zone % 3) {
			case 0: pct_remove = 20; pct_create = 60; break;
			case 1: pct_remove = 33; pct_create = 33; break;
			case 2: pct_remove = 60; pct_create = 20; break;
			}
		}
		i = xfs_random(&rnd) % G007_NAMES;
		op = xfs_random(&rnd) % 100;
		if (op > pct_remove + pct_create)
			g007_lookup(test, i);
		else if (op > pct_remove)
			g007_create(test, i);
		else
			g007_remove(test, i);
	}

	/* -z: remove everything that is left */
	for (i = 0; i < G007_NAMES; i++) {
		int err;

		if (!g007_tab[i].exists)
			continue;
		cleanup++;
		err = xfs_unlink(g007_name(buf, i));
		KUNIT_EXPECT_EQ_MSG(test, err, 0,
				    "\"%s\"(%llu) not removed at cleanup: %d",
				    buf, g007_tab[i].ino, err);
	}

	/* generic/007.out */
	KUNIT_EXPECT_EQ(test, g007_n.good_adds, 18736);
	KUNIT_EXPECT_EQ(test, g007_n.bad_adds, 18802);
	KUNIT_EXPECT_EQ(test, g007_n.good_rms, 18675);
	KUNIT_EXPECT_EQ(test, g007_n.bad_rms, 19927);
	KUNIT_EXPECT_EQ(test, g007_n.good_looks, 12000);
	KUNIT_EXPECT_EQ(test, g007_n.bad_looks, 11860);
	KUNIT_EXPECT_EQ(test, cleanup, 61);
}

static int g007_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g007_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g007_cases[] = {
	KUNIT_CASE_SLOW(nametest_matches_the_model_and_the_golden_counts),
	{}
};

static struct kunit_suite g007_suite = {
	.name		= "xfstests/generic/007",
	.suite_init	= g007_suite_init,
	.suite_exit	= g007_suite_exit,
	.test_cases	= g007_cases,
};

kunit_test_suites(&g007_suite);

MODULE_DESCRIPTION("xfstests generic/007 over a loopback NFS mount");
MODULE_LICENSE("GPL");
