// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/006 over a loopback NFS mount: permname.
 *
 * The original runs src/permname -c 4 -l 6: create a file for every
 * length-6 name over the alphabet {a,b,c,d} -- all 4096 permutations in
 * one directory -- then counts them with find. Over NFS that is 4096
 * CREATE RPCs into one directory, then 4096 LOOKUPs, then 4096 REMOVEs:
 * a large-directory namespace exercise for the client dcache and the
 * server's directory handling.
 *
 * Deviations: upstream also repeats the run with 4 forked processes to
 * shake out concurrency; a KUnit case is single-threaded, so only the
 * single-thread half is ported. Upstream counts via find/readdir; the
 * port verifies by looking up every expected name (same name set, checked
 * from the other direction) plus an O_EXCL collision probe.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G006_ROOT	XFS_MNT "/g006"
#define G006_ASIZE	4	/* alphabet size, as upstream -c 4 */
#define G006_LEN	6	/* name length, as upstream -l 6 */
#define G006_TOTAL	4096	/* 4^6 */

static const char *g006_name(char *buf, unsigned int idx)
{
	int i;
	char *p;

	memcpy(buf, G006_ROOT "/", sizeof(G006_ROOT));
	p = buf + sizeof(G006_ROOT);	/* points just past the '/' */
	for (i = 0; i < G006_LEN; i++)
		p[i] = 'a' + ((idx >> (2 * i)) & (G006_ASIZE - 1));
	p[G006_LEN] = '\0';
	return buf;
}

static void g006_remove_tree(void *unused)
{
	char buf[64];
	unsigned int idx;

	for (idx = 0; idx < G006_TOTAL; idx++)
		xfs_unlink(g006_name(buf, idx));
	xfs_rmdir(G006_ROOT);
}

static void all_4096_permuted_names_coexist_in_one_directory(struct kunit *test)
{
	char buf[64];
	struct file *f;
	unsigned int idx;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G006_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g006_remove_tree,
						  NULL), 0);

	/* permname's mkf(): creat every permutation, each exactly once */
	for (idx = 0; idx < G006_TOTAL; idx++) {
		f = filp_open(g006_name(buf, idx),
			      O_WRONLY | O_CREAT | O_EXCL, 0666);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
				       "creat #%u (%s) failed: %ld",
				       idx, buf, PTR_ERR(f));
		filp_close(f, NULL);
	}

	/* "4097 files created": every name must now exist... */
	for (idx = 0; idx < G006_TOTAL; idx++)
		KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(g006_name(buf, idx)),
				      "%s vanished from the directory", buf);

	/* ...exactly once: a repeat O_EXCL create must collide */
	f = filp_open(g006_name(buf, G006_TOTAL / 2),
		      O_WRONLY | O_CREAT | O_EXCL, 0666);
	KUNIT_ASSERT_TRUE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ_MSG(test, PTR_ERR(f), (long)-EEXIST,
			    "recreating an existing permutation: %ld",
			    PTR_ERR(f));

	/* and the directory must empty back out cleanly */
	for (idx = 0; idx < G006_TOTAL; idx++)
		KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(g006_name(buf, idx)), 0,
				    "unlink %s failed", buf);
	KUNIT_EXPECT_EQ_MSG(test, xfs_rmdir(G006_ROOT), 0,
			    "directory not empty after removing every name");
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G006_ROOT), 0);	/* for the action */
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
	KUNIT_CASE_SLOW(all_4096_permuted_names_coexist_in_one_directory),
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
