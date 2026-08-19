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
 * existence and inode number.
 *
 * Over NFS this pits the client's dcache (positive and negative entries)
 * and inode-number handling (fileids from GETATTR) against the server's
 * truth across thousands of CREATE/REMOVE/LOOKUP RPCs. A stale negative
 * dentry, a mis-cached fileid, or a lost REMOVE shows up as a model
 * mismatch.
 *
 * Deviations: 20,000 iterations rather than upstream's 100,000 (documented
 * cut; the phase pattern below cycles the directory through the same
 * grow/shrink shape). Upstream drifts its create/remove mix over time
 * ("zones"); the port uses three explicit phases -- balanced, create-heavy,
 * remove-heavy -- with a 40% lookup share throughout.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G007_ROOT	XFS_MNT "/g007"
#define G007_NAMES	100	/* as upstream */
#define G007_ITERS	20000	/* upstream: 100000 */
#define G007_SEED	1	/* as upstream */

static struct {
	bool	exists;
	u64	ino;
} g007_tab[G007_NAMES];

static const char *g007_name(char *buf, int i)
{
	/* upstream's input file: nametest.1 .. nametest.100 */
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
		KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
				       "\"%s\" created, but already existed as inumber %llu",
				       buf, g007_tab[i].ino);
		KUNIT_ASSERT_EQ(test, xfs_kstat(buf, &st), 0);
		g007_tab[i].exists = true;
		g007_tab[i].ino = st.ino;
		return;
	}
	KUNIT_ASSERT_EQ_MSG(test, PTR_ERR(f), (long)-EEXIST,
			    "create \"%s\": unexpected error %ld", buf,
			    PTR_ERR(f));
	KUNIT_ASSERT_TRUE_MSG(test, g007_tab[i].exists,
			      "create \"%s\" failed EEXIST, but it should not exist",
			      buf);
}

/* auto_remove(): unlink, checked against the model. */
static void g007_remove(struct kunit *test, int i)
{
	char buf[64];
	int err = xfs_unlink(g007_name(buf, i));

	if (!err) {
		KUNIT_ASSERT_TRUE_MSG(test, g007_tab[i].exists,
				      "\"%s\" removed, should not have existed",
				      buf);
		g007_tab[i].exists = false;
		return;
	}
	KUNIT_ASSERT_EQ_MSG(test, err, -ENOENT,
			    "remove \"%s\": unexpected error %d", buf, err);
	KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
			       "remove \"%s\" failed ENOENT, but it should exist",
			       buf);
}

/* auto_lookup(): stat, checked for existence and inode number. */
static void g007_lookup(struct kunit *test, int i)
{
	char buf[64];
	struct kstat st;
	int err = xfs_kstat(g007_name(buf, i), &st);

	if (!err) {
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
	KUNIT_ASSERT_FALSE_MSG(test, g007_tab[i].exists,
			       "\"%s\" lookup failed ENOENT, but it should exist",
			       buf);
}

static void random_names_stay_consistent_with_the_model(struct kunit *test)
{
	struct rnd_state st;
	int iter;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G007_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g007_remove_tree,
						  NULL), 0);
	memset(g007_tab, 0, sizeof(g007_tab));

	prandom_seed_state(&st, G007_SEED);
	for (iter = 0; iter < G007_ITERS; iter++) {
		u32 r = prandom_u32_state(&st);
		int i = (r >> 8) % G007_NAMES;
		u32 op = r % 100;
		u32 create_pct;

		/* balanced, then grow, then shrink -- upstream's "zones" */
		if (iter < G007_ITERS / 3)
			create_pct = 50;
		else if (iter < (2 * G007_ITERS) / 3)
			create_pct = 75;
		else
			create_pct = 25;

		if (op < 40)
			g007_lookup(test, i);
		else if ((op - 40) % 60 < (create_pct * 60) / 100)
			g007_create(test, i);
		else
			g007_remove(test, i);
	}
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
	KUNIT_CASE_SLOW(random_names_stay_consistent_with_the_model),
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
