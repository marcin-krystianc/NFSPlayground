// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/089 over a loopback NFS mount: mount(8)'s /etc/mtab
 * update, raced.
 *
 * src/t_mtab repeats update_mtab() the way old mount(8) did, in a
 * directory holding a file t_mtab:
 *
 *	lock_mtab(): create t_mtab~<pid>, link() it to t_mtab~ and unlink it;
 *	    if the link won, F_SETLK the lock file, else F_SETLKW on it and
 *	    try again -- only the winner of the link holds the lock
 *	copy t_mtab into a fresh t_mtab.tmp, fchmod it 0644, chown it to
 *	    t_mtab's owner, and rename t_mtab.tmp over t_mtab
 *	unlock_mtab(): unlink t_mtab~
 *
 * generic/089 runs three t_mtab processes of 50 iterations at once, then
 * one of 10000, requires each to report "completed N iterations" and the
 * directory to hold no mtab name but t_mtab; then does it all again after
 * adding 100 entries with 4-character names, and again after 1000 more
 * with 8-character names.
 *
 * Over NFS each iteration is a LINK racing other LINKs to the same name,
 * REMOVEs, an OPEN of a file that may already be gone, LOCK/LOCKW, and a
 * RENAME that replaces the target inode -- in a directory whose size and
 * readdir cache grow between rounds.
 *
 * Each t_mtab process is a kthread working on absolute paths, with the
 * worker's index standing in for its pid. POSIX locks are owned by the
 * open file, and closing it with that owner releases them, as close(2)
 * does for the process that took them. The mtab contents are a fixed
 * mount table in place of upstream's _mount output.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G089_DIR	XFS_MNT "/g089/test"
#define G089_MTAB	G089_DIR "/t_mtab"
#define G089_LOCK	G089_DIR "/t_mtab~"
#define G089_TMP	G089_DIR "/t_mtab.tmp"
#define G089_BUF	4096

static const char g089_mounts[] =
	"proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
	"nfsd /proc/fs/nfsd nfsd rw,relatime 0 0\n"
	"tmpfs /export tmpfs rw,relatime,size=65536k 0 0\n"
	"127.0.0.1:/export /mnt/nfs nfs4 rw,relatime,vers=4.2,sec=sys 0 0\n";

struct g089_worker {
	int			id;
	int			iterations;
	int			completed;
	int			err;
	const char		*what;
	char			*buf;
	struct completion	done;
};

static int g089_fail(struct g089_worker *w, int err, const char *what)
{
	w->err = err;
	w->what = what;
	return err;
}

/* lock_mtab() */
static int g089_lock_mtab(struct g089_worker *w)
{
	char linktarget[64];
	struct file *f;
	int j, err;

	snprintf(linktarget, sizeof(linktarget), G089_LOCK "%d", w->id);
	for (;;) {
		f = filp_open(linktarget, O_WRONLY | O_CREAT, 0);
		if (IS_ERR(f))
			return g089_fail(w, PTR_ERR(f), "can't create lock file");
		filp_close(f, NULL);
		j = xfs_link(linktarget, G089_LOCK);
		xfs_unlink(linktarget);
		if (j && j != -EEXIST)
			return g089_fail(w, j, "can't link lock file");

		f = filp_open(G089_LOCK, O_WRONLY, 0);
		if (IS_ERR(f)) {
			/* strange... maybe the file was just deleted? */
			if (PTR_ERR(f) == -ENOENT)
				continue;
			return g089_fail(w, PTR_ERR(f), "can't open lock file");
		}
		if (!j) {
			/* we made the link: claim the lock */
			err = xfs_posix_lock(f, F_WRLCK, 0, OFFSET_MAX, f, false);
			if (err && err != -EAGAIN && err != -EBUSY) {
				filp_close(f, f);
				return g089_fail(w, err, "can't lock lock file");
			}
			filp_close(f, f);
			return 0;
		}
		/* someone else made the link: wait for their lock */
		err = xfs_posix_lock(f, F_WRLCK, 0, OFFSET_MAX, f, true);
		filp_close(f, f);
		if (err && err != -EAGAIN && err != -EBUSY)
			return g089_fail(w, err, "can't lock lock file");
		cond_resched();
	}
}

/* update_mtab() */
static int g089_update_mtab(struct g089_worker *w)
{
	struct file *in, *out;
	struct kstat st;
	loff_t rpos = 0, wpos = 0;
	ssize_t n;
	int err;

	err = g089_lock_mtab(w);
	if (err)
		return err;

	in = filp_open(G089_MTAB, O_RDONLY, 0);
	if (IS_ERR(in))
		return g089_fail(w, PTR_ERR(in), "cannot open t_mtab for reading");
	out = filp_open(G089_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (IS_ERR(out)) {
		filp_close(in, NULL);
		return g089_fail(w, PTR_ERR(out), "cannot open t_mtab.tmp for writing");
	}
	while ((n = kernel_read(in, w->buf, G089_BUF, &rpos)) > 0) {
		if (kernel_write(out, w->buf, n, &wpos) != n) {
			n = -EIO;
			break;
		}
	}
	filp_close(in, NULL);
	if (n < 0) {
		filp_close(out, NULL);
		return g089_fail(w, n, "read/write failure");
	}
	err = vfs_fchmod(out, 0644);
	filp_close(out, NULL);
	if (err)
		return g089_fail(w, err, "error changing mode of t_mtab.tmp");

	/* copy uid/gid from the present mtab before renaming */
	if (!xfs_kstat(G089_MTAB, &st))
		xfs_chown(G089_TMP, from_kuid(&init_user_ns, st.uid),
			  from_kgid(&init_user_ns, st.gid));

	err = xfs_rename(G089_TMP, G089_MTAB);
	if (err)
		return g089_fail(w, err, "can't rename t_mtab.tmp to t_mtab");

	/* unlock_mtab() */
	err = xfs_unlink(G089_LOCK);
	if (err)
		return g089_fail(w, err, "Cannot remove lock file");
	return 0;
}

/* t_mtab <iterations> */
static int g089_t_mtab(void *arg)
{
	struct g089_worker *w = arg;
	int i;

	for (i = 0; i < w->iterations; i++) {
		if (g089_update_mtab(w))
			break;
		w->completed++;
		cond_resched();
	}
	complete(&w->done);
	return 0;
}

static void g089_start(struct kunit *test, struct g089_worker *w, int id,
		       int iterations)
{
	struct task_struct *t;

	*w = (struct g089_worker){ .id = id, .iterations = iterations };
	w->buf = kunit_kmalloc(test, G089_BUF, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w->buf);
	init_completion(&w->done);
	t = kthread_run(g089_t_mtab, w, "g089-%d", id);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld", PTR_ERR(t));
}

static void g089_expect_completed(struct kunit *test, struct g089_worker *w)
{
	wait_for_completion(&w->done);
	KUNIT_EXPECT_EQ_MSG(test, w->err, 0, "t_mtab %d: %s: %d", w->id,
			    w->what, w->err);
	KUNIT_EXPECT_EQ_MSG(test, w->completed, w->iterations,
			    "t_mtab %d completed %d of %d iterations", w->id,
			    w->completed, w->iterations);
}

/* "ls | grep mtab" must print t_mtab and nothing else */
static void g089_expect_only_t_mtab(struct kunit *test)
{
	struct xfs_dirent *ents;
	struct file *d;
	int i, n, mtabs = 0, t_mtab = 0;

	ents = kunit_kcalloc(test, 64, sizeof(*ents), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);
	d = filp_open(G089_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "opendir: %ld", PTR_ERR(d));
	while ((n = xfs_getdents(d, ents, 64, 32768)) > 0) {
		for (i = 0; i < n; i++) {
			if (!strstr(ents[i].name, "mtab"))
				continue;
			mtabs++;
			if (!strcmp(ents[i].name, "t_mtab"))
				t_mtab++;
			else
				KUNIT_FAIL(test, "directory entry %s left behind",
					   ents[i].name);
		}
	}
	filp_close(d, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, t_mtab, 1);
	KUNIT_EXPECT_EQ(test, mtabs, 1);
	kunit_kfree(test, ents);
}

/* mtab(): three t_mtab 50 at once, then one t_mtab 10000 */
static void g089_mtab(struct kunit *test)
{
	struct g089_worker w[3], big;
	int i;

	for (i = 0; i < 3; i++)
		g089_start(test, &w[i], i, 50);
	for (i = 0; i < 3; i++)
		g089_expect_completed(test, &w[i]);
	g089_start(test, &big, 3, 10000);
	g089_expect_completed(test, &big);
	g089_expect_only_t_mtab(test);
}

/* addentries count width: touch %0<width>d for count down to 1 */
static void g089_addentries(struct kunit *test, int count, int width)
{
	char path[96];

	for (; count > 0; count--) {
		snprintf(path, sizeof(path), G089_DIR "/%0*d", width, count);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(path, "", 0), 0,
				    "touch %s", path);
	}
}

static void g089_remove_tree(void *unused)
{
	char path[96];
	int i;

	for (i = 1; i <= 100; i++) {
		snprintf(path, sizeof(path), G089_DIR "/%04d", i);
		xfs_unlink(path);
	}
	for (i = 1; i <= 1000; i++) {
		snprintf(path, sizeof(path), G089_DIR "/%08d", i);
		xfs_unlink(path);
	}
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), G089_LOCK "%d", i);
		xfs_unlink(path);
	}
	xfs_unlink(G089_LOCK);
	xfs_unlink(G089_TMP);
	xfs_unlink(G089_MTAB);
	xfs_rmdir_settled(G089_DIR);
	xfs_rmdir_settled(XFS_MNT "/g089");
}

static void racing_mtab_updates_leave_only_t_mtab(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/g089"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G089_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g089_remove_tree, NULL),
			0);
	/* _mount > t_mtab */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G089_MTAB, g089_mounts,
						 sizeof(g089_mounts) - 1), 0);

	/* directory with only a few entries */
	g089_mtab(test);
	/* directory with a hundred more entries, each 4chars wide */
	g089_addentries(test, 100, 4);
	g089_mtab(test);
	/* directory with a thousand more entries, each 8chars wide */
	g089_addentries(test, 1000, 8);
	g089_mtab(test);
}

static int g089_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g089_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g089_cases[] = {
	KUNIT_CASE_SLOW(racing_mtab_updates_leave_only_t_mtab),
	{}
};

static struct kunit_suite g089_suite = {
	.name		= "xfstests/generic/089",
	.suite_init	= g089_suite_init,
	.suite_exit	= g089_suite_exit,
	.test_cases	= g089_cases,
};

kunit_test_suites(&g089_suite);

MODULE_DESCRIPTION("xfstests generic/089 over a loopback NFS mount");
MODULE_LICENSE("GPL");
