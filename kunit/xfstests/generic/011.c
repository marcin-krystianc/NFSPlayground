// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/011 over a loopback NFS mount: dirstress.
 *
 * Upstream runs src/dirstress three times with -f 1000:
 *
 *	TEST 1: -p 1 -n 1	one process in its own directory
 *	TEST 2: -p 5 -n 1	five processes, one directory each
 *	TEST 3: -p 5 -n 5	five processes sharing one directory
 *
 * Each process makes $dir/stressdir/stress.<n>, creates 1000 entries in
 * it -- regular files, directories, symlinks to their own name and
 * character device nodes, by i % 4 -- then "scrambles" them with 2000
 * random renames, unlinks, rmdirs, creat()s and mkdirs, and finally
 * removes whatever is left, type-aware, after an lstat. Without -c every
 * individual failure is only printed; the pass criterion is that
 * dirstress exits 0, i.e. nothing crashes or wedges.
 *
 * Over NFS the scramble is the interesting part: RENAME storms across
 * entry types against the client dcache, REMOVE/RMDIR of names whose type
 * just changed, CREATE over freshly deleted names -- and in TEST 3 five
 * workers doing it to the same directory at once.
 *
 * dirstress seeds random() once and then forks, so every child replays
 * the same sequence; the workers here do the same with one seed each run.
 * Each process is a kthread working on absolute paths, since a kthread
 * has no cwd of its own to chdir.
 *
 * Checks beyond upstream's: every worker must get some operations through
 * (a storm where nothing succeeds tested nothing); errors outside the set
 * a scramble can legitimately produce are reported; and in TESTs 1 and 2,
 * where one worker owns its directory, stress.<n> must rmdir cleanly
 * afterwards. TEST 3 can leave names behind by design -- another worker
 * may recreate one after this worker's remove pass has gone by -- which is
 * why upstream only rm -rf's it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G011_ROOT	XFS_MNT "/g011"
#define G011_STRESS	G011_ROOT "/stressdir"
#define G011_NFILES	1000	/* -f 1000, as upstream */
#define G011_MAXPROCS	5

struct g011_worker {
	int			dirnum;
	unsigned int		seed;
	int			ok;
	int			bad_err;	/* first unexpected errno */
	struct completion	done;
};

static void g011_name(char *buf, size_t len, int dirnum, long i)
{
	snprintf(buf, len, G011_STRESS "/stress.%d/XXXXXXXXXXXX.%ld", dirnum,
		 i);
}

/* failures a scramble is expected to hit; anything else is reported */
static void g011_note(struct g011_worker *w, int err)
{
	if (!err) {
		w->ok++;
		return;
	}
	switch (err) {
	case -ENOENT: case -EEXIST: case -EISDIR: case -ENOTDIR:
	case -ENOTEMPTY: case -ELOOP: case -ENXIO: case -ENODEV:
	case -ESTALE:
		return;
	}
	if (!w->bad_err)
		w->bad_err = err;
}

/* creat(2): O_CREAT | O_WRONLY | O_TRUNC */
static int g011_creat(const char *path)
{
	struct file *f = filp_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);

	if (IS_ERR(f))
		return PTR_ERR(f);
	filp_close(f, NULL);
	return 0;
}

static void g011_create_entries(struct g011_worker *w)
{
	char buf[128], target[32];
	int i, err = 0;

	for (i = 0; i < G011_NFILES; i++) {
		g011_name(buf, sizeof(buf), w->dirnum, i);
		switch (i % 4) {
		case 0:
			err = g011_creat(buf);
			break;
		case 1:
			err = xfs_mkdir(buf);
			break;
		case 2:
			/* symlink(buf, buf): the relative name, pointing at itself */
			snprintf(target, sizeof(target), "XXXXXXXXXXXX.%d", i);
			err = xfs_symlink(target, buf);
			break;
		case 3:
			err = xfs_mknod(buf, S_IFCHR | 0666, 0, 0);	/* MKNOD_DEV */
			break;
		}
		g011_note(w, err);
	}
}

static void g011_scramble_entries(struct g011_worker *w, struct xfs_random *r)
{
	char buf[128], buf1[128];
	int i, err = 0;

	for (i = 0; i < G011_NFILES * 2; i++) {
		switch (i % 5) {
		case 0:
			g011_name(buf, sizeof(buf), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			g011_name(buf1, sizeof(buf1), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			err = xfs_rename(buf, buf1);
			break;
		case 1:
			g011_name(buf, sizeof(buf), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			err = xfs_unlink(buf);
			break;
		case 2:
			g011_name(buf, sizeof(buf), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			err = xfs_rmdir(buf);
			break;
		case 3:
			g011_name(buf, sizeof(buf), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			err = g011_creat(buf);
			break;
		case 4:
			g011_name(buf, sizeof(buf), w->dirnum,
				  xfs_random(r) % G011_NFILES);
			err = xfs_mkdir(buf);
			break;
		}
		g011_note(w, err);
		cond_resched();
	}
}

/* lstat each name and rmdir or unlink by type */
static void g011_remove_entries(struct g011_worker *w)
{
	struct kstat st;
	char buf[128];
	int i;

	for (i = 0; i < G011_NFILES; i++) {
		g011_name(buf, sizeof(buf), w->dirnum, i);
		if (xfs_kstat(buf, &st))
			continue;
		g011_note(w, S_ISDIR(st.mode) ? xfs_rmdir(buf) : xfs_unlink(buf));
	}
}

/* dirstress(): one child */
static int g011_dirstress(void *arg)
{
	struct g011_worker *w = arg;
	struct xfs_random r;
	char dir[64];
	int err;

	xfs_srandom(&r, w->seed);
	err = xfs_mkdir(G011_STRESS);
	if (err && err != -EEXIST)
		w->bad_err = err;
	snprintf(dir, sizeof(dir), G011_STRESS "/stress.%d", w->dirnum);
	err = xfs_mkdir(dir);
	if (err && err != -EEXIST)
		w->bad_err = err;
	if (!w->bad_err) {
		g011_create_entries(w);
		g011_scramble_entries(w, &r);
		g011_remove_entries(w);
	}
	complete(&w->done);
	return 0;
}

static void g011_remove_tree(void *unused)
{
	char buf[128];
	struct kstat st;
	int d, i;

	for (d = 0; d < G011_MAXPROCS; d++) {
		for (i = 0; i < G011_NFILES; i++) {
			g011_name(buf, sizeof(buf), d, i);
			if (!xfs_kstat(buf, &st))
				S_ISDIR(st.mode) ? xfs_rmdir(buf) : xfs_unlink(buf);
		}
		snprintf(buf, sizeof(buf), G011_STRESS "/stress.%d", d);
		xfs_rmdir_settled(buf);
	}
	xfs_rmdir_settled(G011_STRESS);
	xfs_rmdir_settled(G011_ROOT);
}

/* _test(): dirstress -d $out -f 1000 -p nprocs -n nprocs_per_dir */
static void g011_run(struct kunit *test, int nprocs, int per_dir)
{
	struct g011_worker w[G011_MAXPROCS] = {};
	unsigned int seed = get_random_u32();
	struct task_struct *t;
	char dir[64];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G011_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g011_remove_tree,
						  NULL), 0);
	kunit_info(test, "dirstress -f %d -p %d -n %d, seed %u\n",
		   G011_NFILES, nprocs, per_dir, seed);

	for (i = 0; i < nprocs; i++) {
		w[i].dirnum = i / per_dir;
		w[i].seed = seed;
		init_completion(&w[i].done);
		t = kthread_run(g011_dirstress, &w[i], "g011-%d", i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
	}
	for (i = 0; i < nprocs; i++)
		wait_for_completion(&w[i].done);

	for (i = 0; i < nprocs; i++) {
		KUNIT_EXPECT_EQ_MSG(test, w[i].bad_err, 0,
				    "worker %d hit an unexpected error %d", i,
				    w[i].bad_err);
		KUNIT_EXPECT_GT_MSG(test, w[i].ok, G011_NFILES / 4,
				    "worker %d: only %d operations succeeded",
				    i, w[i].ok);
	}

	/*
	 * Not upstream: a directory one worker owned must now be empty.
	 * Settled, because unlinking or renaming over a just-closed file can
	 * leave a transient .nfsXXXX entry until the delayed fput lands.
	 */
	if (per_dir == 1) {
		for (i = 0; i < nprocs; i++) {
			snprintf(dir, sizeof(dir), G011_STRESS "/stress.%d", i);
			KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir_settled(dir), 0,
					    "stress.%d is not empty after remove_entries",
					    i);
		}
	}
}

static void test_1_one_process(struct kunit *test)
{
	g011_run(test, 1, 1);
}

static void test_2_five_processes_five_directories(struct kunit *test)
{
	g011_run(test, 5, 1);
}

static void test_3_five_processes_one_directory(struct kunit *test)
{
	g011_run(test, 5, 5);
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
	KUNIT_CASE_SLOW(test_1_one_process),
	KUNIT_CASE_SLOW(test_2_five_processes_five_directories),
	KUNIT_CASE_SLOW(test_3_five_processes_one_directory),
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
