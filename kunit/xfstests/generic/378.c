// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/378 over a loopback NFS mount: a hard link shares its
 * inode's permissions.
 *
 * Upstream writes a file, hard-links it, removes read permission from the
 * first name, and then has an unprivileged user cat both names. Its
 * golden output is two "Permission denied" lines: the chmod applies to
 * the inode, so it must deny the other name too. Overlayfs once gave each
 * link its own inode and failed exactly this.
 *
 * Over NFS the two names are two dentries the client resolved separately,
 * each with its own cached attributes, and the SETATTR went to only one
 * of them. The client has to notice that the other name's inode is the
 * same one -- nfs_find_actor() matches on the filehandle, not the name --
 * or it will happily serve the stale mode and let the read through.
 *
 * Deviations: the read is attempted with capabilities dropped and as a
 * user who is neither owner nor group, which is what _user_do arranges;
 * "cat" becomes an open for reading, since that is where the permission
 * check happens. The port also checks nlink and the reverse direction
 * (restoring the mode through the second name unblocks the first),
 * neither of which upstream does.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G378_ROOT	XFS_MNT "/g378"
#define G378_FILE	G378_ROOT "/testfile.378"
#define G378_LINK	G378_ROOT "/testfile.378.hardlink"

#define G378_UID	1000
#define G378_GID	1000

static void g378_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G378_LINK);
	xfs_unlink(G378_FILE);
	xfs_rmdir_settled(G378_ROOT);
}

/* open for reading as an unprivileged user; returns 0 or -errno */
static int g378_read_as_user(const char *path)
{
	struct file *f;
	int err;

	err = xfs_switch_creds(G378_UID, G378_GID);
	if (err)
		return err;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
	} else {
		err = 0;
		filp_close(f, NULL);
	}
	xfs_restore_creds();
	return err;
}

static void a_hard_link_shares_the_inodes_mode(struct kunit *test)
{
	static const char content[] = "You should not see this\n";
	struct kstat st;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G378_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g378_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G378_FILE, content,
					   sizeof(content) - 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G378_FILE, 0644), 0);
	KUNIT_ASSERT_EQ(test, xfs_link(G378_FILE, G378_LINK), 0);

	/* both names are readable while the mode allows it */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G378_LINK, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.nlink, 2U, "nlink is %u", st.nlink);
	KUNIT_EXPECT_EQ_MSG(test, g378_read_as_user(G378_FILE), 0,
			    "the file was not readable to start with");
	KUNIT_EXPECT_EQ_MSG(test, g378_read_as_user(G378_LINK), 0,
			    "the link was not readable to start with");

	/* chmod -r on one name */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G378_FILE, 0200), 0);

	KUNIT_EXPECT_EQ_MSG(test, g378_read_as_user(G378_FILE), -EACCES,
			    "the file stayed readable after chmod -r");
	KUNIT_EXPECT_EQ_MSG(test, g378_read_as_user(G378_LINK), -EACCES,
			    "the hard link stayed readable after chmod -r on the other name");

	/* and the other way round: the link's chmod reaches the first name */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G378_LINK, 0644), 0);
	KUNIT_EXPECT_EQ_MSG(test, g378_read_as_user(G378_FILE), 0,
			    "restoring the mode through the link did not reach the file");
}

static int g378_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g378_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g378_cases[] = {
	KUNIT_CASE(a_hard_link_shares_the_inodes_mode),
	{}
};

static struct kunit_suite g378_suite = {
	.name		= "xfstests/generic/378",
	.suite_init	= g378_suite_init,
	.suite_exit	= g378_suite_exit,
	.test_cases	= g378_cases,
};

kunit_test_suites(&g378_suite);

MODULE_DESCRIPTION("xfstests generic/378 over a loopback NFS mount");
MODULE_LICENSE("GPL");
