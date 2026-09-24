// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/131 over a loopback NFS mount: POSIX advisory locks.
 *
 * Upstream runs src/locktest as a server and a client process, each with
 * its own O_RDWR descriptor of one file, and steps both through the
 * lock_tests[] table in order: F_SETLK read locks, write locks and unlocks
 * over ranges chosen to hit every case of lock-list insertion, splitting,
 * merging and overlap, first within one process and then across the two;
 * locks past EOF and over the whole file; a close that must drop the
 * closing process's locks; and an F_GETLK for a write lock on a read-only
 * descriptor. Each step expects PASS or FAIL, and any mismatch fails the
 * test. Over NFSv4 every one of them is protocol state: LOCK, LOCKT and
 * LOCKU against the server's lockowners.
 *
 * The table below is lock_tests[] as Linux compiles it (the macosx-only
 * tests 30-32 excluded): 221 steps, copied row for row. The two processes
 * are two open files with two lock owners in this thread; closing a side
 * releases the locks that side owns, as close(2) does for a process.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>

#include "xfstests_nfs_fixture.h"

#define G131_ROOT	XFS_MNT "/g131"
#define G131_FILE	G131_ROOT "/lockfile"

#define FILE_SIZE	1024	/* locktest.c */

enum {
	CMD_WRLOCK, CMD_RDLOCK, CMD_UNLOCK, CMD_CLOSE, CMD_OPEN, CMD_WRTEST,
	CMD_RDTEST,
};
enum { PASS, FAIL };
enum { SERVER, CLIENT };

/* test#, command, offset (or open flags), length, expected, who */
static const s64 g131_lock_tests[][6] = {
	{ 1, CMD_WRLOCK, 1, 10, PASS, SERVER },
	{ 1, CMD_UNLOCK, 1, 10, PASS, SERVER },
	{ 2, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 2, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 2, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 2, CMD_WRLOCK, 1, 5, PASS, SERVER },
	{ 2, CMD_WRLOCK, 70, 5, PASS, SERVER },
	{ 2, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 2, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 2, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 2, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 2, CMD_UNLOCK, 70, 5, PASS, SERVER },
	{ 3, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 3, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 3, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 3, CMD_WRLOCK, 42, 5, PASS, SERVER },
	{ 3, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 3, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 3, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 3, CMD_UNLOCK, 42, 5, PASS, SERVER },
	{ 4, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 4, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 4, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_RDLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 4, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 4, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 5, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 5, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 5, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 5, CMD_WRLOCK, 30, 15, PASS, SERVER },
	{ 5, CMD_WRLOCK, 25, 20, PASS, SERVER },
	{ 5, CMD_WRLOCK, 22, 26, PASS, SERVER },
	{ 5, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 5, CMD_UNLOCK, 22, 26, PASS, SERVER },
	{ 5, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 6, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 6, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 6, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 6, CMD_WRLOCK, 30, 5, PASS, SERVER },
	{ 6, CMD_WRLOCK, 32, 6, PASS, SERVER },
	{ 6, CMD_WRLOCK, 32, 8, PASS, SERVER },
	{ 6, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 6, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 6, CMD_UNLOCK, 32, 8, PASS, SERVER },
	{ 6, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 7, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 7, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 7, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 7, CMD_WRLOCK, 27, 10, PASS, SERVER },
	{ 7, CMD_WRLOCK, 25, 2, PASS, SERVER },
	{ 7, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 7, CMD_UNLOCK, 25, 15, PASS, SERVER },
	{ 7, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 8, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 8, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 8, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 8, CMD_WRLOCK, 35, 10, PASS, SERVER },
	{ 8, CMD_WRLOCK, 45, 2, PASS, SERVER },
	{ 8, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 8, CMD_UNLOCK, 30, 17, PASS, SERVER },
	{ 8, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 9, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 9, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 9, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 9, CMD_RDLOCK, 30, 15, PASS, SERVER },
	{ 9, CMD_WRLOCK, 25, 20, PASS, SERVER },
	{ 9, CMD_RDLOCK, 22, 26, PASS, SERVER },
	{ 9, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 9, CMD_UNLOCK, 22, 26, PASS, SERVER },
	{ 9, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 10, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 10, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 10, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 10, CMD_RDLOCK, 30, 5, PASS, SERVER },
	{ 10, CMD_WRLOCK, 32, 2, PASS, SERVER },
	{ 10, CMD_RDLOCK, 36, 5, PASS, SERVER },
	{ 10, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 10, CMD_UNLOCK, 30, 11, PASS, SERVER },
	{ 10, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 11, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 11, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 11, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 11, CMD_RDLOCK, 27, 10, PASS, SERVER },
	{ 11, CMD_WRLOCK, 25, 3, PASS, SERVER },
	{ 11, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 11, CMD_UNLOCK, 25, 15, PASS, SERVER },
	{ 11, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 12, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 12, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 12, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 12, CMD_RDLOCK, 35, 10, PASS, SERVER },
	{ 12, CMD_WRLOCK, 44, 3, PASS, SERVER },
	{ 12, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 12, CMD_UNLOCK, 30, 18, PASS, SERVER },
	{ 12, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 13, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 13, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 13, CMD_RDLOCK, 50, 10, PASS, SERVER },
	{ 13, CMD_WRLOCK, 30, 10, FAIL, CLIENT },
	{ 13, CMD_RDLOCK, 50, 10, PASS, CLIENT },
	{ 13, CMD_RDLOCK, 30, 10, FAIL, CLIENT },
	{ 13, CMD_UNLOCK, 30, 10, PASS, CLIENT },
	{ 13, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 13, CMD_WRLOCK, 30, 10, PASS, CLIENT },
	{ 13, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 13, CMD_UNLOCK, 30, 10, PASS, CLIENT },
	{ 13, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 14, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 14, CMD_WRLOCK, 30, 10, PASS, SERVER },
	{ 14, CMD_RDLOCK, 50, 10, PASS, SERVER },
	{ 14, CMD_RDLOCK, 30, 15, FAIL, CLIENT },
	{ 14, CMD_WRLOCK, 30, 15, FAIL, CLIENT },
	{ 14, CMD_RDLOCK, 25, 20, FAIL, CLIENT },
	{ 14, CMD_WRLOCK, 25, 20, FAIL, CLIENT },
	{ 14, CMD_RDLOCK, 22, 26, FAIL, CLIENT },
	{ 14, CMD_WRLOCK, 22, 26, FAIL, CLIENT },
	{ 14, CMD_RDLOCK, 50, 15, PASS, CLIENT },
	{ 14, CMD_WRLOCK, 50, 17, FAIL, CLIENT },
	{ 14, CMD_RDLOCK, 45, 20, PASS, CLIENT },
	{ 14, CMD_WRLOCK, 43, 22, FAIL, CLIENT },
	{ 14, CMD_RDLOCK, 42, 26, PASS, CLIENT },
	{ 14, CMD_WRLOCK, 41, 28, FAIL, CLIENT },
	{ 14, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 14, CMD_UNLOCK, 22, 26, PASS, SERVER },
	{ 14, CMD_UNLOCK, 42, 26, PASS, CLIENT },
	{ 15, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 15, CMD_RDLOCK, 30, 10, PASS, SERVER },
	{ 15, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 15, CMD_RDLOCK, 50, 5, FAIL, CLIENT },
	{ 15, CMD_WRLOCK, 50, 5, FAIL, CLIENT },
	{ 15, CMD_RDLOCK, 52, 6, FAIL, CLIENT },
	{ 15, CMD_WRLOCK, 52, 6, FAIL, CLIENT },
	{ 15, CMD_RDLOCK, 52, 8, FAIL, CLIENT },
	{ 15, CMD_WRLOCK, 52, 8, FAIL, CLIENT },
	{ 15, CMD_RDLOCK, 30, 5, PASS, CLIENT },
	{ 15, CMD_WRLOCK, 30, 5, FAIL, CLIENT },
	{ 15, CMD_RDLOCK, 32, 6, PASS, CLIENT },
	{ 15, CMD_WRLOCK, 32, 6, FAIL, CLIENT },
	{ 15, CMD_RDLOCK, 32, 8, PASS, CLIENT },
	{ 15, CMD_WRLOCK, 32, 8, FAIL, CLIENT },
	{ 15, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 15, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 15, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 16, CMD_RDLOCK, 10, 10, PASS, SERVER },
	{ 16, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 16, CMD_RDLOCK, 5, 6, PASS, CLIENT },
	{ 16, CMD_WRLOCK, 5, 6, FAIL, CLIENT },
	{ 16, CMD_RDLOCK, 5, 10, PASS, CLIENT },
	{ 16, CMD_WRLOCK, 5, 10, FAIL, CLIENT },
	{ 16, CMD_RDLOCK, 45, 6, FAIL, CLIENT },
	{ 16, CMD_WRLOCK, 45, 6, FAIL, CLIENT },
	{ 16, CMD_RDLOCK, 45, 10, FAIL, CLIENT },
	{ 16, CMD_WRLOCK, 45, 10, FAIL, CLIENT },
	{ 16, CMD_UNLOCK, 5, 15, PASS, CLIENT },
	{ 16, CMD_UNLOCK, 30, 10, PASS, SERVER },
	{ 16, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 17, CMD_WRLOCK, 10, 10, PASS, SERVER },
	{ 17, CMD_RDLOCK, 30, 10, PASS, SERVER },
	{ 17, CMD_WRLOCK, 50, 10, PASS, SERVER },
	{ 17, CMD_WRLOCK, 35, 10, FAIL, CLIENT },
	{ 17, CMD_RDLOCK, 35, 10, PASS, CLIENT },
	{ 17, CMD_RDLOCK, 44, 2, PASS, CLIENT },
	{ 17, CMD_RDLOCK, 55, 10, FAIL, CLIENT },
	{ 17, CMD_WRLOCK, 55, 10, FAIL, CLIENT },
	{ 17, CMD_RDLOCK, 59, 5, FAIL, CLIENT },
	{ 17, CMD_WRLOCK, 59, 5, FAIL, CLIENT },
	{ 17, CMD_UNLOCK, 10, 10, PASS, SERVER },
	{ 17, CMD_UNLOCK, 30, 16, PASS, CLIENT },
	{ 17, CMD_UNLOCK, 50, 10, PASS, SERVER },
	{ 18, CMD_WRLOCK, 11, 7, PASS, SERVER },
	{ 18, CMD_WRLOCK, 13, 8, FAIL, CLIENT },
	{ 18, CMD_UNLOCK, 11, 7, PASS, SERVER },
	{ 19, CMD_WRLOCK, 10, FILE_SIZE, PASS, SERVER },
	{ 19, CMD_WRLOCK, FILE_SIZE + 10, 10, PASS, CLIENT },
	{ 19, CMD_UNLOCK, 10, FILE_SIZE, PASS, SERVER },
	{ 19, CMD_UNLOCK, FILE_SIZE + 10, 10, PASS, CLIENT },
	{ 20, CMD_WRLOCK, 10, FILE_SIZE, PASS, SERVER },
	{ 20, CMD_WRLOCK, 10, FILE_SIZE, FAIL, CLIENT },
	{ 20, CMD_UNLOCK, 10, FILE_SIZE, PASS, SERVER },
	{ 21, CMD_WRLOCK, 0, 0, PASS, SERVER },
	{ 21, CMD_WRLOCK, 0, 0, FAIL, CLIENT },
	{ 21, CMD_UNLOCK, 0, 0, PASS, SERVER },
	{ 22, CMD_WRLOCK, 0, 0, PASS, SERVER },
	{ 22, CMD_WRLOCK, 1, 5, FAIL, CLIENT },
	{ 22, CMD_UNLOCK, 0, 0, PASS, SERVER },
	{ 23, CMD_RDLOCK, 1, 5, PASS, SERVER },
	{ 23, CMD_RDLOCK, 7, 6, PASS, CLIENT },
	{ 23, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 23, CMD_UNLOCK, 7, 6, PASS, CLIENT },
	{ 24, CMD_RDLOCK, 1, 5, PASS, SERVER },
	{ 24, CMD_RDLOCK, 2, 6, PASS, CLIENT },
	{ 24, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 24, CMD_UNLOCK, 1, 7, PASS, CLIENT },
	{ 25, CMD_RDLOCK, 1, 5, PASS, SERVER },
	{ 25, CMD_WRLOCK, 7, 6, PASS, CLIENT },
	{ 25, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 25, CMD_UNLOCK, 7, 6, PASS, CLIENT },
	{ 26, CMD_RDLOCK, 1, 5, PASS, SERVER },
	{ 26, CMD_WRLOCK, 2, 6, FAIL, CLIENT },
	{ 26, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 27, CMD_WRLOCK, 0, 0, PASS, SERVER },
	{ 27, CMD_WRLOCK, 1, 5, FAIL, CLIENT },
	{ 27, CMD_CLOSE, 0, 0, PASS, SERVER },
	{ 27, CMD_WRLOCK, 1, 5, PASS, CLIENT },
	{ 27, CMD_OPEN, O_RDWR, 0, PASS, SERVER },
	{ 27, CMD_UNLOCK, 1, 5, PASS, CLIENT },
	{ 28, CMD_RDLOCK, 1, 5, PASS, SERVER },
	{ 28, CMD_RDLOCK, 1, 5, PASS, CLIENT },
	{ 28, CMD_CLOSE, 0, 0, PASS, SERVER },
	{ 28, CMD_OPEN, O_RDWR, 0, PASS, SERVER },
	{ 28, CMD_WRLOCK, 0, 0, FAIL, SERVER },
	{ 28, CMD_UNLOCK, 1, 5, PASS, SERVER },
	{ 29, CMD_CLOSE, 0, 0, PASS, SERVER },
	{ 29, CMD_OPEN, O_RDONLY, 0, PASS, SERVER },
	{ 29, CMD_WRTEST, 0, 0, PASS, SERVER },
	{ 29, CMD_CLOSE, 0, 0, PASS, SERVER },
	{ 29, CMD_OPEN, O_RDWR, 0, PASS, SERVER },
};

struct g131_side {
	struct file	*f;	/* f_fd; NULL when closed */
};

static void g131_remove_tree(void *unused)
{
	xfs_unlink(G131_FILE);
	xfs_rmdir_settled(G131_ROOT);
}

/* do_open(): the flags plus O_CREAT, mode 0666 */
static int g131_open(struct g131_side *s, int flags)
{
	s->f = filp_open(G131_FILE, flags | O_CREAT, 0666);
	if (IS_ERR(s->f)) {
		s->f = NULL;
		return FAIL;
	}
	return PASS;
}

/* do_close(): close(2), which drops this process's locks on the file */
static int g131_close(struct g131_side *s)
{
	if (!s->f)
		return FAIL;
	filp_close(s->f, s);
	s->f = NULL;
	return PASS;
}

/* do_lock(): fcntl(F_SETLK or F_GETLK) with l_whence SEEK_SET */
static int g131_lock(struct g131_side *s, bool test, unsigned char type,
		     s64 start, s64 len)
{
	loff_t end = len ? start + len - 1 : OFFSET_MAX;
	struct file_lock *fl;
	int err;

	if (!s->f)
		return FAIL;
	if (!test)
		return xfs_posix_lock(s->f, type, start, end, s, false) ?
			FAIL : PASS;

	fl = locks_alloc_lock();
	if (!fl)
		return FAIL;
	fl->c.flc_type = type;
	fl->c.flc_flags = FL_POSIX;
	fl->c.flc_owner = s;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_file = s->f;
	fl->fl_start = start;
	fl->fl_end = end;
	err = vfs_test_lock(s->f, fl);
	locks_free_lock(fl);
	return err ? FAIL : PASS;
}

static void the_lock_tests_table_runs_as_upstream(struct kunit *test)
{
	struct g131_side sides[2] = {};
	int i, result;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G131_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g131_remove_tree, NULL),
			0);

	/* both processes start with do_open(O_RDWR) */
	KUNIT_ASSERT_EQ(test, g131_open(&sides[SERVER], O_RDWR), PASS);
	KUNIT_ASSERT_EQ(test, g131_open(&sides[CLIENT], O_RDWR), PASS);

	for (i = 0; i < ARRAY_SIZE(g131_lock_tests); i++) {
		const s64 *t = g131_lock_tests[i];
		struct g131_side *s = &sides[t[5]];

		switch (t[1]) {
		case CMD_WRLOCK:
			result = g131_lock(s, false, F_WRLCK, t[2], t[3]);
			break;
		case CMD_RDLOCK:
			result = g131_lock(s, false, F_RDLCK, t[2], t[3]);
			break;
		case CMD_UNLOCK:
			result = g131_lock(s, false, F_UNLCK, t[2], t[3]);
			break;
		case CMD_CLOSE:
			result = g131_close(s);
			break;
		case CMD_OPEN:
			result = g131_open(s, t[2]);
			break;
		case CMD_WRTEST:
			result = g131_lock(s, true, F_WRLCK, t[2], t[3]);
			break;
		default:
			result = g131_lock(s, true, F_RDLCK, t[2], t[3]);
			break;
		}
		KUNIT_EXPECT_EQ_MSG(test, result, (int)t[4],
				    "test %lld, step %d: %s %s at %lld, length %lld: %s, expected %s",
				    t[0], i, t[5] == SERVER ? "server" : "client",
				    t[1] == CMD_WRLOCK ? "write lock" :
				    t[1] == CMD_RDLOCK ? "read lock" :
				    t[1] == CMD_UNLOCK ? "unlock" :
				    t[1] == CMD_CLOSE ? "close" :
				    t[1] == CMD_OPEN ? "open" : "F_GETLK",
				    t[2], t[3], result == PASS ? "PASS" : "FAIL",
				    t[4] == PASS ? "PASS" : "FAIL");
	}

	g131_close(&sides[SERVER]);
	g131_close(&sides[CLIENT]);
}

static int g131_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g131_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g131_cases[] = {
	KUNIT_CASE(the_lock_tests_table_runs_as_upstream),
	{}
};

static struct kunit_suite g131_suite = {
	.name		= "xfstests/generic/131",
	.suite_init	= g131_suite_init,
	.suite_exit	= g131_suite_exit,
	.test_cases	= g131_cases,
};

kunit_test_suites(&g131_suite);

MODULE_DESCRIPTION("xfstests generic/131 over a loopback NFS mount");
MODULE_LICENSE("GPL");
