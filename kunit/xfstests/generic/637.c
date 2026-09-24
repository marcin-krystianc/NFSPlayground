// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/637 over a loopback NFS mount: a directory modified
 * while it is being read.
 *
 * src/t_dir_offset2 <dir> 200 {+name|-name} opens the directory, reads
 * one 200-byte getdents batch (fewer than 10 entries), creates or unlinks
 * the name, and finishes the walk on the old descriptor. It then walks a
 * descriptor opened after the change, which must show the entry if and
 * only if it exists. Every walk fails on a repeated d_off, and at the end
 * lseek to each recorded d_off must return the entry recorded after it.
 * generic/637 runs it with "+0" on an empty directory, creates files 1 to
 * 100, and runs it with "-10", "-20", ... "-100".
 *
 * Over NFS every one of those is a question about cookies: d_off is the
 * server's cookie, the client caches whole pages of entries keyed on it,
 * and a modification bumps the directory's change attribute, which is
 * what makes a newly opened handle refill from the server. A cookie that
 * repeats, or a fresh handle that serves the old cached pages, is the
 * failure this catches.
 *
 * xfs_t_dir_offset2() is t_dir_offset2 with its checks as KUnit
 * expectations. The files are created with an in-kernel open and close,
 * whose final fput is deferred; the port settles those before the unlinks
 * so that none turns into a sillyrename.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G637_ROOT	XFS_MNT "/g637"
#define G637_DIR	G637_ROOT "/test-637"
#define G637_BUFSIZE	200	/* fits fewer than 10 entries */

static void g637_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_settle_fput();
	for (i = 0; i <= 100; i++) {
		snprintf(path, sizeof(path), G637_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G637_DIR);
	xfs_rmdir_settled(G637_ROOT);
}

static void readdir_of_a_directory_changed_while_open(struct kunit *test)
{
	char path[64], arg[8];
	int n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G637_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g637_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G637_DIR), 0);

	/* Create file 0 in an open dir */
	xfs_t_dir_offset2(test, G637_DIR, G637_BUFSIZE, "+0");

	for (n = 1; n <= 100; n++) {
		snprintf(path, sizeof(path), G637_DIR "/%d", n);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, NULL, 0), 0);
	}
	xfs_settle_fput();

	/* Remove file ${n}0 in an open dir */
	for (n = 1; n <= 10; n++) {
		snprintf(arg, sizeof(arg), "-%d0", n);
		xfs_t_dir_offset2(test, G637_DIR, G637_BUFSIZE, arg);
	}
}

static int g637_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g637_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g637_cases[] = {
	KUNIT_CASE(readdir_of_a_directory_changed_while_open),
	{}
};

static struct kunit_suite g637_suite = {
	.name		= "xfstests/generic/637",
	.suite_init	= g637_suite_init,
	.suite_exit	= g637_suite_exit,
	.test_cases	= g637_cases,
};

kunit_test_suites(&g637_suite);

MODULE_DESCRIPTION("xfstests generic/637 over a loopback NFS mount");
MODULE_LICENSE("GPL");
