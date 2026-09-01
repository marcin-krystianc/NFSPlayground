// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/002 over a loopback NFS mount: the inode link count.
 *
 * The original creates a file, hard-links it up to 20 names checking
 * `Links:` from lstat64 after every link, then walks back down checking
 * before every removal. Over NFS every step is a LINK or REMOVE RPC plus a
 * GETATTR, so what is really under test is the client's nlink handling:
 * the post-op attribute update (or invalidation) after LINK/REMOVE, and
 * revalidation on stat. xfs_kstat() uses AT_STATX_FORCE_SYNC, so each
 * check consults the server rather than trusting a stale cache.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G002_ROOT	XFS_MNT "/g002"
#define G002_LINKS	20	/* as upstream */

static const char *g002_name(char *buf, int i)
{
	snprintf(buf, 64, G002_ROOT "/tmp.%d", i);
	return buf;
}

static void g002_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 1; i <= G002_LINKS; i++)
		xfs_unlink(g002_name(buf, i));
	xfs_rmdir(G002_ROOT);
}

static void link_count_follows_every_link_and_unlink(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	char buf[64], first[64];
	int l;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G002_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g002_remove_tree,
						  NULL), 0);

	g002_name(first, 1);
	f = filp_open(first, O_WRONLY | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(first, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, 1U, "fresh file must have 1 link");

	/* up: after creating link #l the count must be exactly l */
	for (l = 2; l <= G002_LINKS; l++) {
		KUNIT_ASSERT_EQ_MSG(test, xfs_link(first, g002_name(buf, l)),
				    0, "creating link #%d failed", l);
		KUNIT_ASSERT_EQ(test, xfs_kstat(first, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.nlink, (unsigned int)l,
				    "created link #%d but the count disagrees",
				    l);
	}

	/* down: before removing link #l the count must still be l */
	for (l = G002_LINKS; l >= 1; l--) {
		KUNIT_ASSERT_EQ(test, xfs_kstat(first, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.nlink, (unsigned int)l,
				    "about to remove link #%d but the count disagrees",
				    l);
		KUNIT_ASSERT_EQ_MSG(test, xfs_unlink(g002_name(buf, l)), 0,
				    "removing link #%d failed", l);
	}

	KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(first),
			       "the file outlived the removal of its last link");
}

static int g002_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g002_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g002_cases[] = {
	KUNIT_CASE(link_count_follows_every_link_and_unlink),
	{}
};

static struct kunit_suite g002_suite = {
	.name		= "xfstests/generic/002",
	.suite_init	= g002_suite_init,
	.suite_exit	= g002_suite_exit,
	.test_cases	= g002_cases,
};

kunit_test_suites(&g002_suite);

MODULE_DESCRIPTION("xfstests generic/002 over a loopback NFS mount");
MODULE_LICENSE("GPL");
