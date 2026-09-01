// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/013 over a loopback NFS mount: fsstress.
 *
 * The original runs ltp/fsstress, a randomized filesystem exerciser whose
 * pass criterion is survival: thousands of random operations, nothing
 * checked beyond "no crash, no error from the harness, tree removable".
 * Porting fsstress wholesale is out of scope; this is a deliberately
 * reduced mini-fsstress with the same shape: a seeded random storm of
 * mixed namespace and data operations over one name pool, where individual
 * ops may legitimately fail (ENOENT, EEXIST, EISDIR, ENOTEMPTY...) and the
 * suite asserts three things -- no unexpected errno class, enough ops
 * actually succeeded for the storm to mean anything, and the tree comes
 * apart cleanly afterwards.
 *
 * Over NFS with the kernel's UBSAN/dmesg-clean CI gate, "survival" is a
 * real check: every op is an RPC, and a client bug under this mix
 * (rename-over-live-name, rmdir of a non-empty directory, truncate racing
 * append) shows up as a crash, a wedged mount, or a leftover entry.
 *
 * Deviations: 2,000 ops, single-threaded, 10 op types (upstream fsstress
 * has ~50 including xattr/clone/ioctl variants and multi-process runs).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G013_ROOT	XFS_MNT "/g013"
#define G013_NAMES	40
#define G013_OPS	2000
#define G013_SEED	13

static const char *g013_name(char *buf, int i)
{
	snprintf(buf, 64, G013_ROOT "/p%02d", i);
	return buf;
}

static int g013_remove_entry(const char *path)
{
	int err = xfs_unlink(path);

	if (err == -EISDIR)
		err = xfs_rmdir(path);
	return err;
}

static void g013_remove_tree(void *unused)
{
	char buf[64];
	int i, pass;

	/* symlink/link webs may need more than one sweep */
	for (pass = 0; pass < 3; pass++)
		for (i = 0; i < G013_NAMES; i++)
			g013_remove_entry(g013_name(buf, i));
	/* settled: sillyrename entries may still be in flight, see 011 */
	xfs_rmdir_settled(G013_ROOT);
}

/* errno classes a random storm may legitimately produce */
static bool g013_errno_expected(int err)
{
	switch (err) {
	case 0:
	case -ENOENT: case -EEXIST: case -EISDIR: case -ENOTDIR:
	case -ENOTEMPTY: case -ELOOP: case -EINVAL: case -EBUSY:
	case -EPERM:	/* hard link of a directory */
	case -EMLINK:	/* link count ceiling */
		return true;
	default:
		return false;
	}
}

static void mini_fsstress_survives_a_random_op_storm(struct kunit *test)
{
	struct rnd_state st;
	struct kstat kst;
	char a[64], b[64];
	struct file *f;
	u8 *data;
	int i, err, ok = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G013_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g013_remove_tree,
						  NULL), 0);

	data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, data);
	memset(data, 0x5A, 4096);

	prandom_seed_state(&st, G013_SEED);
	for (i = 0; i < G013_OPS; i++) {
		u32 r = prandom_u32_state(&st);
		int x = (r >> 8) % G013_NAMES;
		int y = (r >> 16) % G013_NAMES;

		err = 0;
		switch (r % 10) {
		case 0:	/* creat */
			f = filp_open(g013_name(a, x),
				      O_WRONLY | O_CREAT | O_EXCL, 0644);
			if (IS_ERR(f))
				err = PTR_ERR(f);
			else
				filp_close(f, NULL);
			break;
		case 1:	/* append up to 4K */
			f = filp_open(g013_name(a, x), O_WRONLY | O_APPEND, 0);
			if (IS_ERR(f)) {
				err = PTR_ERR(f);
			} else {
				loff_t pos = 0;
				size_t n = 1 + (r >> 20) % 4096;
				ssize_t w = kernel_write(f, data, n, &pos);

				if (w < 0)
					err = w;
				filp_close(f, NULL);
			}
			break;
		case 2:	/* mkdir */
			err = xfs_mkdir(g013_name(a, x));
			break;
		case 3:	/* rmdir */
			err = xfs_rmdir(g013_name(a, x));
			break;
		case 4:	/* unlink */
			err = xfs_unlink(g013_name(a, x));
			break;
		case 5:	/* rename */
			err = xfs_rename(g013_name(a, x), g013_name(b, y));
			break;
		case 6:	/* symlink */
			err = xfs_symlink(g013_name(a, y), g013_name(b, x));
			break;
		case 7:	/* hard link */
			err = xfs_link(g013_name(a, x), g013_name(b, y));
			break;
		case 8:	/* truncate to a random size */
			err = xfs_truncate(g013_name(a, x), (r >> 4) % 65536);
			break;
		case 9:	/* stat, forced revalidation */
			err = xfs_kstat(g013_name(a, x), &kst);
			break;
		}
		if (!err)
			ok++;
		KUNIT_ASSERT_TRUE_MSG(test, g013_errno_expected(err),
				      "op %d (kind %u) on %s: unexpected errno %d",
				      i, r % 10, a, err);
	}

	/* vacuity guard: the storm must have actually done things */
	KUNIT_EXPECT_GT_MSG(test, ok, G013_OPS / 4,
			    "only %d of %d ops succeeded", ok, G013_OPS);

	/* and the tree must come apart completely */
	g013_remove_tree(NULL);
	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(G013_ROOT),
			       "the op storm left entries that cannot be removed");
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G013_ROOT), 0);	/* for the action */
}

static int g013_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g013_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g013_cases[] = {
	KUNIT_CASE_SLOW(mini_fsstress_survives_a_random_op_storm),
	{}
};

static struct kunit_suite g013_suite = {
	.name		= "xfstests/generic/013",
	.suite_init	= g013_suite_init,
	.suite_exit	= g013_suite_exit,
	.test_cases	= g013_cases,
};

kunit_test_suites(&g013_suite);

MODULE_DESCRIPTION("xfstests generic/013 over a loopback NFS mount");
MODULE_LICENSE("GPL");
