// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/070 over a loopback NFS mount: xattr storm with a model.
 *
 * Upstream runs fsstress in xattr mode. The port is a model-checked
 * storm: thirty files, ten possible attribute names each, 1500 seeded
 * set/get/remove/list operations where a small model tracks which
 * (file, name) pairs exist and with which generation; every get must
 * match the model exactly and every list must have the model's count.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G070_ROOT	XFS_MNT "/g070"

#define G070_FILES	30
#define G070_NAMES	10
#define G070_OPS	1500

static u16 g070_gen[G070_FILES][G070_NAMES];	/* 0 = absent */

static void g070_paths(char *file, char *name, int f, int a)
{
	snprintf(file, 64, G070_ROOT "/x%02d", f);
	snprintf(name, 32, "user.a%02d", a);
}

static void g070_remove_tree(void *unused)
{
	char file[64];
	int f;

	for (f = 0; f < G070_FILES; f++) {
		snprintf(file, sizeof(file), G070_ROOT "/x%02d", f);
		xfs_unlink(file);
	}
	xfs_rmdir(G070_ROOT);
}

static void xattr_storm_matches_the_model(struct kunit *test)
{
	struct rnd_state st;
	char file[64], name[32], val[32], want[32], list[512];
	int op, f, a, err;
	ssize_t n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G070_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g070_remove_tree, NULL),
			0);
	memset(g070_gen, 0, sizeof(g070_gen));

	for (f = 0; f < G070_FILES; f++) {
		snprintf(file, sizeof(file), G070_ROOT "/x%02d", f);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(file, "s", 1), 0);
	}

	/* capability probe once */
	g070_paths(file, name, 0, 0);
	err = xfs_setxattr(file, name, "p", 1, 0);
	if (err == -EOPNOTSUPP)
		kunit_skip(test, "user xattrs unsupported here");
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, xfs_removexattr(file, name), 0);

	prandom_seed_state(&st, 70);
	for (op = 0; op < G070_OPS; op++) {
		u32 r = prandom_u32_state(&st);

		f = (r >> 8) % G070_FILES;
		a = (r >> 16) % G070_NAMES;
		g070_paths(file, name, f, a);

		switch (r % 4) {
		case 0:	case 1:		/* set (create or replace) */
			g070_gen[f][a]++;
			snprintf(val, sizeof(val), "g%u", g070_gen[f][a]);
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_setxattr(file, name, val,
							 strlen(val), 0), 0,
					    "op %d: set %s on %s", op, name,
					    file);
			break;
		case 2:			/* get-verify against the model */
			n = xfs_getxattr(file, name, val, sizeof(val));
			if (g070_gen[f][a] == 0) {
				KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)-ENODATA,
						    "op %d: absent attr returned %zd",
						    op, n);
			} else {
				snprintf(want, sizeof(want), "g%u",
					 g070_gen[f][a]);
				KUNIT_ASSERT_EQ_MSG(test, n,
						    (ssize_t)strlen(want),
						    "op %d: wrong length", op);
				KUNIT_ASSERT_EQ_MSG(test,
						    memcmp(val, want, n), 0,
						    "op %d: stale generation",
						    op);
			}
			break;
		case 3:			/* remove */
			err = xfs_removexattr(file, name);
			if (g070_gen[f][a] == 0)
				KUNIT_ASSERT_EQ(test, err, -ENODATA);
			else
				KUNIT_ASSERT_EQ_MSG(test, err, 0,
						    "op %d: remove failed %d",
						    op, err);
			g070_gen[f][a] = 0;
			break;
		}

		if (op % 100 == 99) {	/* list count must match the model */
			int have = 0, i;
			const char *p;

			for (i = 0; i < G070_NAMES; i++)
				if (g070_gen[f][i])
					have++;
			n = xfs_listxattr(file, list, sizeof(list));
			KUNIT_ASSERT_GE(test, n, (ssize_t)0);
			for (p = list, i = 0; p < list + n; p += strlen(p) + 1)
				i++;
			KUNIT_ASSERT_EQ_MSG(test, i, have,
					    "op %d: list has %d names, model %d",
					    op, i, have);
		}
	}
}

static int g070_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g070_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g070_cases[] = {
	KUNIT_CASE_SLOW(xattr_storm_matches_the_model),
	{}
};

static struct kunit_suite g070_suite = {
	.name		= "xfstests/generic/070",
	.suite_init	= g070_suite_init,
	.suite_exit	= g070_suite_exit,
	.test_cases	= g070_cases,
};

kunit_test_suites(&g070_suite);

MODULE_DESCRIPTION("xfstests generic/070 over a loopback NFS mount");
MODULE_LICENSE("GPL");
