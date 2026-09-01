// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/314 over a loopback NFS mount: SGID directory inheritance.
 *
 * Upstream: entries created inside a setgid directory inherit its group;
 * subdirectories also inherit the setgid bit. Created here over NFS by an
 * identity whose own gid differs, so the inheritance decision is the
 * server's (CREATE/MKDIR against tmpfs), observed through GETATTR.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G314_ROOT	XFS_MNT "/g314"

static void g314_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G314_PARENT	G314_ROOT "/sgid"

static void g314_remove_tree(void *unused)
{
	xfs_unlink(G314_PARENT "/file");
	xfs_rmdir(G314_PARENT "/subdir");
	xfs_rmdir(G314_PARENT);
	xfs_rmdir(G314_ROOT);
}

static void sgid_group_and_bit_are_inherited(struct kunit *test)
{
	struct kstat st;
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_PARENT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G314_PARENT, 0, 12345), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G314_PARENT, 02777), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G314_PARENT, &st), 0);
	KUNIT_ASSERT_TRUE_MSG(test, st.mode & S_ISGID,
			      "the parent lost its setgid bit (%o)", st.mode);

	/* create as an identity whose gid is decidedly not 12345 */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(100, 500), 0);
	f = filp_open(G314_PARENT "/file", O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (!IS_ERR(f))
		filp_close(f, NULL);
	KUNIT_EXPECT_FALSE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G314_PARENT "/subdir"), 0);
	xfs_restore_creds();

	KUNIT_ASSERT_EQ(test, xfs_kstat(G314_PARENT "/file", &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, (int)__kgid_val(KGIDT_INIT(0)) * 0 +
			    (int)st.gid.val, 12345,
			    "the file's group is %d, not the sgid parent's",
			    (int)st.gid.val);
	KUNIT_EXPECT_EQ_MSG(test, (int)st.uid.val, 100,
			    "the file's owner is %d, not the creator",
			    (int)st.uid.val);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G314_PARENT "/subdir", &st), 0);
	KUNIT_EXPECT_EQ(test, (int)st.gid.val, 12345);
	KUNIT_EXPECT_TRUE_MSG(test, st.mode & S_ISGID,
			      "the subdirectory did not inherit setgid (%o)",
			      st.mode);
}

static int g314_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g314_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g314_cases[] = {
	KUNIT_CASE(sgid_group_and_bit_are_inherited),
	{}
};

static struct kunit_suite g314_suite = {
	.name		= "xfstests/generic/314",
	.suite_init	= g314_suite_init,
	.suite_exit	= g314_suite_exit,
	.test_cases	= g314_cases,
};

kunit_test_suites(&g314_suite);

MODULE_DESCRIPTION("xfstests generic/314 over a loopback NFS mount");
MODULE_LICENSE("GPL");
