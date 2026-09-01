// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/011 over a loopback NFS mount: dirstress.
 *
 * src/dirstress fills a directory with mixed entry types -- regular files,
 * subdirectories, self-targeted symlinks and char device nodes -- then
 * "scrambles" it with random renames, unlinks, rmdirs and re-creates
 * where individual operations are allowed to fail (renaming a directory
 * over a file, removing a name twice), and finally removes everything.
 * The pass criterion upstream is simply that dirstress exits 0.
 *
 * Over NFS the scramble is the interesting part: RENAME storms across
 * entry types against the client dcache, REMOVE/RMDIR of names whose type
 * just changed, CREATE over freshly deleted names. NFSv4.2 carries every
 * type used here (device nodes are NF4CHR creates).
 *
 * Deviations: single-threaded (upstream's TEST 2/3 rerun the same logic in
 * 5 forked processes for concurrency coverage); the port adds a final
 * assertion upstream lacks -- after remove_entries the directory must
 * rmdir cleanly, proving nothing leaked.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>

#include "xfstests_nfs_fixture.h"

#define G011_ROOT	XFS_MNT "/g011"
#define G011_NFILES	64	/* upstream TEST 1: -f 100, one process */
#define G011_SEED	1

static const char *g011_name(char *buf, int i)
{
	/* upstream's literal name pattern */
	snprintf(buf, 64, G011_ROOT "/XXXXXXXXXXXX.%d", i);
	return buf;
}

/* type-aware removal: try file/symlink/device first, then directory */
static int g011_remove_entry(const char *path)
{
	int err = xfs_unlink(path);

	if (err == -EISDIR)
		err = xfs_rmdir(path);
	return err;
}

static void g011_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G011_NFILES; i++)
		g011_remove_entry(g011_name(buf, i));
	xfs_rmdir_settled(G011_ROOT);
}

/* create_entries(): one entry per name, type cycling by i % 4 */
static void g011_create_entries(struct kunit *test)
{
	char buf[64];
	struct file *f;
	int i, err;

	for (i = 0; i < G011_NFILES; i++) {
		g011_name(buf, i);
		switch (i % 4) {
		case 0:	/* regular file */
			f = filp_open(buf, O_WRONLY | O_CREAT | O_EXCL, 0666);
			KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
					       "creat %s: %ld", buf,
					       PTR_ERR(f));
			filp_close(f, NULL);
			break;
		case 1:	/* directory */
			KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(buf), 0,
					    "mkdir %s failed", buf);
			break;
		case 2:	/* symlink to its own name, as upstream */
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_symlink(buf, buf), 0,
					    "symlink %s failed", buf);
			break;
		case 3:	/* char device node */
			err = xfs_mknod_chr(buf);
			KUNIT_ASSERT_EQ_MSG(test, err, 0,
					    "mknod %s failed: %d", buf, err);
			break;
		}
	}
}

/*
 * scramble_entries(): 2*nfiles random ops; failures are expected and
 * tolerated (upstream runs without -c), but the storm must not wedge.
 */
static void g011_scramble_entries(struct kunit *test)
{
	struct rnd_state st;
	char a[64], b[64];
	struct file *f;
	int i, ok = 0;

	prandom_seed_state(&st, G011_SEED);
	for (i = 0; i < 2 * G011_NFILES; i++) {
		u32 r = prandom_u32_state(&st);
		int x = (r >> 8) % G011_NFILES;
		int y = (r >> 16) % G011_NFILES;

		switch (i % 5) {
		case 0:	/* rename random -> random */
			if (!xfs_rename(g011_name(a, x), g011_name(b, y)))
				ok++;
			break;
		case 1:	/* unlink random */
			if (!xfs_unlink(g011_name(a, x)))
				ok++;
			break;
		case 2:	/* rmdir random */
			if (!xfs_rmdir(g011_name(a, x)))
				ok++;
			break;
		case 3:	/* create random */
			f = filp_open(g011_name(a, x),
				      O_WRONLY | O_CREAT | O_EXCL, 0666);
			if (!IS_ERR(f)) {
				filp_close(f, NULL);
				ok++;
			}
			break;
		case 4:	/* mkdir random */
			if (!xfs_mkdir(g011_name(a, x)))
				ok++;
			break;
		}
	}
	/* vacuity guard: a scramble where nothing succeeded tested nothing */
	KUNIT_EXPECT_GT_MSG(test, ok, G011_NFILES / 4,
			    "only %d of %d scramble ops succeeded", ok,
			    2 * G011_NFILES);
}

/* remove_entries(): everything must go, whatever type it ended up as */
static void g011_remove_entries(struct kunit *test)
{
	char buf[64];
	int i, err;

	for (i = 0; i < G011_NFILES; i++) {
		err = g011_remove_entry(g011_name(buf, i));
		KUNIT_ASSERT_TRUE_MSG(test, err == 0 || err == -ENOENT,
				      "removing %s failed: %d", buf, err);
	}
}

static void dirstress_create_scramble_remove(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G011_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g011_remove_tree,
						  NULL), 0);

	g011_create_entries(test);
	g011_scramble_entries(test);
	g011_remove_entries(test);

	/*
	 * Stronger than upstream: nothing may have leaked. Settled, because
	 * a scramble op that unlinked or renamed over a just-closed file can
	 * leave a transient sillyrename (.nfsXXXX) entry until the delayed
	 * fput lands -- correct NFS client behaviour, not a leak.
	 */
	KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir_settled(G011_ROOT), 0,
			    "the directory is not empty after remove_entries");
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G011_ROOT), 0);	/* for the action */
}

static int g011_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g011_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g011_cases[] = {
	KUNIT_CASE(dirstress_create_scramble_remove),
	{}
};

static struct kunit_suite g011_suite = {
	.name		= "xfstests/generic/011",
	.suite_init	= g011_suite_init,
	.suite_exit	= g011_suite_exit,
	.test_cases	= g011_cases,
};

kunit_test_suites(&g011_suite);

MODULE_DESCRIPTION("xfstests generic/011 over a loopback NFS mount");
MODULE_LICENSE("GPL");
