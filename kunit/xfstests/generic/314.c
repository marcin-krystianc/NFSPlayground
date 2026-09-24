// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/314 over a loopback NFS mount: SGID inheritance on
 * subdirectories.
 *
 * Upstream makes a directory owned by qa_user and an unrelated group
 * (12345), chmods it 2775, and has qa_user -- who is not in group 12345 --
 * mkdir a subdirectory under umask 022. The golden output is the
 * subdirectory's mode, "drwxr-sr-x": 0755 from the mkdir and the umask,
 * plus the setgid bit inherited from the parent.
 *
 * Over NFS the inheritance is the server's decision (MKDIR against tmpfs),
 * observed through GETATTR. The port also checks the subdirectory's group,
 * which setgid inheritance implies but upstream does not print.
 *
 * A second case, not in upstream, creates a regular file in the same kind
 * of directory: it must take the directory's group and its creator's uid.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G314_ROOT	XFS_MNT "/g314"
#define G314_DIR	G314_ROOT "/314-dir"
#define G314_SUBDIR	G314_DIR "/subdir"
#define G314_FILE	G314_DIR "/file"
#define G314_QA_USER	1000
#define G314_QA_GROUP	1000
#define G314_GROUP	12345

static void g314_creds_action(void *unused)
{
	xfs_restore_creds();
}

static void g314_remove_tree(void *unused)
{
	xfs_settle_fput();
	xfs_unlink(G314_FILE);
	xfs_rmdir(G314_SUBDIR);
	xfs_rmdir(G314_DIR);
	xfs_rmdir_settled(G314_ROOT);
}

static void a_subdirectory_inherits_sgid(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_creds_action, NULL),
			0);

	/* dir owned by qa user, and an unrelated group; made sgid */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G314_DIR, G314_QA_USER, G314_GROUP), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G314_DIR, 02775), 0);

	/* _su $qa_user -c "umask 022; mkdir $TEST_DIR/$seq-dir/subdir" */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(G314_QA_USER, G314_QA_GROUP), 0);
	KUNIT_EXPECT_EQ(test, xfs_mkdir(G314_SUBDIR), 0);
	xfs_restore_creds();

	/* drwxr-sr-x subdir */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G314_SUBDIR, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.mode, (umode_t)(S_IFDIR | 02755),
			    "subdir mode is %o, expected drwxr-sr-x (%o)",
			    st.mode, S_IFDIR | 02755);
	KUNIT_EXPECT_EQ_MSG(test, from_kgid(&init_user_ns, st.gid),
			    (gid_t)G314_GROUP,
			    "subdir group is %u, not the sgid parent's",
			    from_kgid(&init_user_ns, st.gid));
}

/* not in upstream: a regular file takes the sgid directory's group */
static void a_file_inherits_the_group(struct kunit *test)
{
	struct kstat st;
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g314_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G314_DIR), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G314_DIR, G314_QA_USER, G314_GROUP), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G314_DIR, 02775), 0);

	KUNIT_ASSERT_EQ(test, xfs_switch_creds(G314_QA_USER, G314_QA_GROUP), 0);
	f = filp_open(G314_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (!IS_ERR(f))
		filp_close(f, NULL);
	xfs_restore_creds();
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create: %ld", PTR_ERR(f));

	KUNIT_ASSERT_EQ(test, xfs_kstat(G314_FILE, &st), 0);
	KUNIT_EXPECT_EQ(test, from_kgid(&init_user_ns, st.gid),
			(gid_t)G314_GROUP);
	KUNIT_EXPECT_EQ(test, from_kuid(&init_user_ns, st.uid),
			(uid_t)G314_QA_USER);
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
	KUNIT_CASE(a_subdirectory_inherits_sgid),
	KUNIT_CASE(a_file_inherits_the_group),
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
