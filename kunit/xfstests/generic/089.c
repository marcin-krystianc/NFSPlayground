// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/089 over a loopback NFS mount: mtab-style link and rename churn.
 *
 * Upstream emulates how mount(8) maintained /etc/mtab: hard-link the
 * file to mtab~, rename mtab~ back over mtab, repeat. Because both names
 * are the same inode, the rename is the POSIX same-inode no-op, so the
 * link count must go 1 -> 2 -> (still 2) -> 1 every cycle, with every
 * step a LINK/RENAME/REMOVE RPC and the count checked against the server
 * each time. A client that mis-caches nlink across the no-op rename
 * drifts within a few cycles.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G089_ROOT	XFS_MNT "/g089"

#define G089_CYCLES	200
#define G089_MTAB	G089_ROOT "/mtab"
#define G089_MTABT	G089_ROOT "/mtab~"

static void g089_remove_tree(void *unused)
{
	xfs_unlink(G089_MTABT);
	xfs_unlink(G089_MTAB);
	xfs_rmdir(G089_ROOT);
}

static void link_rename_cycles_keep_nlink_exact(struct kunit *test)
{
	struct kstat st;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G089_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g089_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G089_MTAB, "mnt", 3), 0);

	for (i = 0; i < G089_CYCLES; i++) {
		KUNIT_ASSERT_EQ_MSG(test, xfs_link(G089_MTAB, G089_MTABT), 0,
				    "cycle %d: link failed", i);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G089_MTAB, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.nlink, 2U,
				    "cycle %d: nlink after link is %u", i,
				    st.nlink);

		/* same inode: the POSIX no-op, both names must survive */
		KUNIT_ASSERT_EQ_MSG(test, xfs_rename(G089_MTABT, G089_MTAB), 0,
				    "cycle %d: rename failed", i);
		KUNIT_ASSERT_TRUE(test, xfs_exists(G089_MTABT));
		KUNIT_ASSERT_EQ(test, xfs_kstat(G089_MTAB, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.nlink, 2U,
				    "cycle %d: the no-op rename changed nlink to %u",
				    i, st.nlink);

		KUNIT_ASSERT_EQ(test, xfs_unlink(G089_MTABT), 0);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G089_MTAB, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.nlink, 1U,
				    "cycle %d: nlink after unlink is %u", i,
				    st.nlink);
	}
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
	KUNIT_CASE_SLOW(link_rename_cycles_keep_nlink_exact),
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
