// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/127 over a loopback NFS mount: six concurrent fsx runs,
 * with and without mapped I/O.
 *
 * Upstream starts six ltp/fsx processes at once, each on its own file in
 * TEST_DIR, with FSX_ARGS "-q -l 262144 -o 65536 -S 191110531 -N 100000":
 *
 *   fsx_lite_nommap       -L -R -W on a file first filled with 256 KiB of zeroes
 *   fsx_lite_mmap         -L       on a file first filled the same way
 *   fsx_std_nommap        -R -W
 *   fsx_std_mmap
 *   fsx_std_nommap_flush  -f ... -R -W
 *   fsx_std_mmap_flush    -f ...
 *
 * -L creates no file and keeps its size fixed, -R -W turn mapped reads and
 * writes off, -f flushes and invalidates the cache after I/O. fsx checks every
 * read against its own copy of the file and exits non-zero on the first
 * mismatch; the test fails if any of the six does.
 *
 * Over NFS the six files share one client: concurrent writeback, mapped
 * faults through nfs_vm_page_mkwrite() and read-backs, all over one
 * connection to the server.
 *
 * The port runs upstream's fsx as six userspace processes inside the test
 * kernel (xfs_run_prog()), each started from its own kthread, with
 * upstream's arguments.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G127_ROOT	XFS_MNT "/g127"
#define G127_SIZE	262144	/* FSX_FILE_SIZE */
#define G127_RUNS	6

static const struct g127_run {
	const char	*name;
	bool		lite;		/* -L, file pre-filled with zeroes */
	bool		nommap;		/* -R -W */
	bool		flush;		/* -f */
} g127_runs[G127_RUNS] = {
	{ "fsx_lite_nommap",		true,	true,	false },
	{ "fsx_lite_mmap",		true,	false,	false },
	{ "fsx_std_nommap",		false,	true,	false },
	{ "fsx_std_mmap",		false,	false,	false },
	{ "fsx_std_nommap_flush",	false,	true,	true },
	{ "fsx_std_mmap_flush",		false,	false,	true },
};

struct g127_job {
	struct kunit		*test;
	const struct g127_run	*run;
	char			path[64];
	int			ret;
	struct completion	done;
};

static int g127_fsx(void *arg)
{
	struct g127_job *j = arg;
	const struct g127_run *r = j->run;
	const char *args[20];
	int a = 0;

	/* $FSX_PROG [-f] $FSX_ARGS [-L] [-R -W] $fname */
	if (r->flush)
		args[a++] = "-f";
	args[a++] = "-q";
	args[a++] = "-l";
	args[a++] = "262144";
	args[a++] = "-o";
	args[a++] = "65536";
	args[a++] = "-S";
	args[a++] = "191110531";
	args[a++] = "-N";
	args[a++] = "100000";
	if (r->lite)
		args[a++] = "-L";
	if (r->nommap) {
		args[a++] = "-R";
		args[a++] = "-W";
	}
	args[a++] = j->path;
	args[a] = NULL;

	j->ret = xfs_run_prog(j->test, "fsx", args);
	complete(&j->done);
	return 0;
}

static void g127_remove_tree(void *unused)
{
	char path[80];
	int i;

	for (i = 0; i < G127_RUNS; i++) {
		snprintf(path, sizeof(path), G127_ROOT "/%s", g127_runs[i].name);
		xfs_unlink(path);
		strlcat(path, ".fsxlog", sizeof(path));
		xfs_unlink(path);
		snprintf(path, sizeof(path), G127_ROOT "/%s.fsxgood",
			 g127_runs[i].name);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G127_ROOT);
}

static void six_concurrent_fsx_runs(struct kunit *test)
{
	struct g127_job *jobs;
	u8 *zeroes;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G127_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g127_remove_tree, NULL),
			0);

	jobs = kunit_kcalloc(test, G127_RUNS, sizeof(*jobs), GFP_KERNEL);
	zeroes = kunit_kzalloc(test, G127_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, jobs);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, zeroes);

	for (i = 0; i < G127_RUNS; i++) {
		struct g127_job *j = &jobs[i];

		j->test = test;
		j->run = &g127_runs[i];
		snprintf(j->path, sizeof(j->path), G127_ROOT "/%s",
			 j->run->name);
		init_completion(&j->done);
		/* dd if=/dev/zero of=$file bs=$FSX_FILE_SIZE count=1 */
		if (j->run->lite)
			KUNIT_ASSERT_EQ(test,
					xfs_write_new_file(j->path, zeroes,
							   G127_SIZE), 0);
	}
	xfs_settle_fput();

	for (i = 0; i < G127_RUNS; i++) {
		struct task_struct *t;

		t = kthread_run(g127_fsx, &jobs[i], "g127-%s",
				jobs[i].run->name);
		if (IS_ERR(t)) {
			jobs[i].ret = PTR_ERR(t);
			complete(&jobs[i].done);
		}
	}
	for (i = 0; i < G127_RUNS; i++) {
		wait_for_completion(&jobs[i].done);
		KUNIT_EXPECT_EQ_MSG(test, jobs[i].ret, 0, "%s exited with %d",
				    jobs[i].run->name, jobs[i].ret);
	}
}

static int g127_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g127_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g127_cases[] = {
	KUNIT_CASE_SLOW(six_concurrent_fsx_runs),
	{}
};

static struct kunit_suite g127_suite = {
	.name		= "xfstests/generic/127",
	.suite_init	= g127_suite_init,
	.suite_exit	= g127_suite_exit,
	.test_cases	= g127_cases,
};

kunit_test_suites(&g127_suite);

MODULE_DESCRIPTION("xfstests generic/127 over a loopback NFS mount");
MODULE_LICENSE("GPL");
