// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/006 over a loopback NFS mount: permname.
 *
 * Upstream runs src/permname -c 4 -l 6 twice, in fresh directories a and
 * b: it creat()s every length-6 name over the alphabet {a,b,c,d}, all
 * 4096 of them, in one directory, and then counts them with find, which
 * must print 4097 lines (the 4096 names and "."). The first run is one
 * process (-p 1); the second is four (-p 4), each creating the 1024 names
 * whose last character is its own quarter of the alphabet, all at once in
 * the same directory.
 *
 * Over NFS that is 4096 CREATE RPCs into one directory -- from four
 * concurrent creators in the second run -- and then a READDIR walk of the
 * result: a large-directory exercise for the client dcache, the readdir
 * cache and the server's directory handling.
 *
 * Each permname process is a kthread; find's count is a getdents walk of
 * the directory counting every name but "..", with the buffer glibc's
 * readdir(3) uses (xfs_libc_dirbuf()). Beyond upstream, each
 * directory must then empty out and rmdir cleanly.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include "xfstests_nfs_fixture.h"

#define G006_ROOT	XFS_MNT "/g006"
#define G006_ASIZE	4	/* -c 4 */
#define G006_LEN	6	/* -l 6 */
#define G006_TOTAL	4096	/* 4^6 */

struct g006_proc {
	const char		*dir;
	int			p, nproc;
	int			made;
	int			err;
	struct completion	done;
};

/* the name for permutation number n, as the alphabet digits of n */
static void g006_name(char *buf, size_t len, const char *dir, unsigned int n)
{
	char name[G006_LEN + 1];
	int i;

	for (i = G006_LEN - 1; i >= 0; i--, n /= G006_ASIZE)
		name[i] = 'a' + n % G006_ASIZE;
	name[G006_LEN] = '\0';
	snprintf(buf, len, "%s/%s", dir, name);
}

/*
 * mkf(0, p): every prefix, and for the last character only this process's
 * share of the alphabet, alpha[p * asplit .. (p + 1) * asplit)
 */
static int g006_mkf(void *arg)
{
	struct g006_proc *pr = arg;
	int asplit = G006_ASIZE / pr->nproc;
	unsigned int prefix, last;
	char path[64];
	struct file *f;

	for (prefix = 0; prefix < G006_TOTAL / G006_ASIZE && !pr->err; prefix++) {
		for (last = pr->p * asplit; last < (pr->p + 1) * asplit; last++) {
			g006_name(path, sizeof(path), pr->dir,
				  prefix * G006_ASIZE + last);
			f = filp_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
			if (IS_ERR(f)) {
				pr->err = PTR_ERR(f);	/* perror("creat"); exit(1) */
				break;
			}
			filp_close(f, NULL);
			pr->made++;
		}
		cond_resched();
	}
	complete(&pr->done);
	return 0;
}

/* find . | _count */
static int g006_count(struct kunit *test, const char *dir)
{
	struct xfs_dirent *ents;
	struct file *d;
	size_t bufsize;
	int n, i, count = 0;

	/* the whole directory: no getdents64 call can return more */
	ents = kvcalloc(G006_TOTAL + 2, sizeof(*ents), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);
	d = filp_open(dir, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(d)) {
		kvfree(ents);
		KUNIT_FAIL(test, "open %s: %ld", dir, PTR_ERR(d));
		return -1;
	}
	bufsize = xfs_libc_dirbuf(d);
	KUNIT_EXPECT_GT(test, bufsize, 0UL);
	while ((n = xfs_getdents(d, ents, G006_TOTAL + 2, bufsize)) > 0)
		for (i = 0; i < n; i++)
			if (strcmp(ents[i].name, ".."))
				count++;
	filp_close(d, NULL);
	kvfree(ents);
	KUNIT_EXPECT_EQ(test, n, 0);
	return count;
}

static void g006_empty(const char *dir)
{
	char path[64];
	unsigned int n;

	for (n = 0; n < G006_TOTAL; n++) {
		g006_name(path, sizeof(path), dir, n);
		xfs_unlink(path);
	}
}

static void g006_remove_tree(void *unused)
{
	xfs_settle_fput();
	g006_empty(G006_ROOT "/a");
	g006_empty(G006_ROOT "/b");
	xfs_rmdir_settled(G006_ROOT "/a");
	xfs_rmdir_settled(G006_ROOT "/b");
	xfs_rmdir_settled(G006_ROOT);
}

/* mkdir $dir; cd $dir; permname -c 4 -l 6 -p nproc; find . | _count */
static void g006_permname(struct kunit *test, const char *dir, int nproc)
{
	struct g006_proc pr[G006_ASIZE] = {};
	struct task_struct *t;
	int i;

	KUNIT_ASSERT_EQ(test, xfs_mkdir(dir), 0);
	for (i = 0; i < nproc; i++) {
		pr[i] = (struct g006_proc){ .dir = dir, .p = i, .nproc = nproc };
		init_completion(&pr[i].done);
		t = kthread_run(g006_mkf, &pr[i], "g006-%d", i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "fork: %ld", PTR_ERR(t));
	}
	for (i = 0; i < nproc; i++) {
		wait_for_completion(&pr[i].done);
		KUNIT_EXPECT_EQ_MSG(test, pr[i].err, 0,
				    "permname process %d: creat failed: %d", i,
				    pr[i].err);
		KUNIT_EXPECT_EQ(test, pr[i].made, G006_TOTAL / nproc);
	}

	/* "4097 files created" */
	KUNIT_EXPECT_EQ_MSG(test, g006_count(test, dir), G006_TOTAL + 1,
			    "find %s did not count 4097 entries", dir);

	/* not upstream: the directory empties back out */
	xfs_settle_fput();
	g006_empty(dir);
	KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir_settled(dir), 0,
			    "%s not empty after removing every name", dir);
}

static void single_and_multi_thread_permname(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G006_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g006_remove_tree,
						  NULL), 0);

	/* single thread permname */
	g006_permname(test, G006_ROOT "/a", 1);
	/* multi thread permname */
	g006_permname(test, G006_ROOT "/b", 4);
}

static int g006_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g006_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g006_cases[] = {
	KUNIT_CASE_SLOW(single_and_multi_thread_permname),
	{}
};

static struct kunit_suite g006_suite = {
	.name		= "xfstests/generic/006",
	.suite_init	= g006_suite_init,
	.suite_exit	= g006_suite_exit,
	.test_cases	= g006_cases,
};

kunit_test_suites(&g006_suite);

MODULE_DESCRIPTION("xfstests generic/006 over a loopback NFS mount");
MODULE_LICENSE("GPL");
