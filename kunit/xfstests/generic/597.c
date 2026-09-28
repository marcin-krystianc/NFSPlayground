// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/597 over a loopback NFS mount: fs.protected_symlinks
 * and fs.protected_hardlinks.
 *
 * Upstream builds a world-writable sticky directory and a target file
 * outside it, then as qa_user (fsgqa):
 *
 *   - follows a symlink in the sticky directory to the target, with
 *     protected_symlinks 0 then 1. The directory belongs to fsgqa, the
 *     link to root; with protection on, following a link whose owner is
 *     neither the follower nor the directory owner is EACCES
 *     (may_follow_link()).
 *   - hardlinks the target, owned by fsgqa2 and not readable by others,
 *     into the sticky directory, with protected_hardlinks 0 then 1. With
 *     protection on, linking a file the caller neither owns nor can read
 *     and write is EPERM (may_linkat()).
 *
 * Over NFS the checks are the client VFS's, made against the owners and
 * modes the client got from the server in GETATTR, and the hardlink that
 * is allowed is a LINK RPC.
 *
 * Deviations: fsgqa is uid/gid 1000 and fsgqa2 1001, switched to with
 * capabilities dropped (xfs_switch_creds()). The sysctls are written
 * through /proc/sys, with procfs mounted for the test. Upstream's "chown
 * fsgqa2 symlink" follows the link, so the port chowns the target.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>

#include "xfstests_nfs_fixture.h"

#define G597_ROOT	XFS_MNT "/g597"
#define G597_STICKY	G597_ROOT "/sticky_dir"
#define G597_TARGET	G597_ROOT "/target"
#define G597_SYMLINK	G597_STICKY "/symlink"
#define G597_HARDLINK	G597_STICKY "/hardlink"

#define G597_OTHER	1000	/* fsgqa, qa_user */
#define G597_OWNER	1001	/* fsgqa2 */

static const char g597_msg[] = "successfully followed symlink\n";
static char g597_old_symlinks[16], g597_old_hardlinks[16];

/* sysctl -n / sysctl -w on /proc/sys/fs/<name> */
static int g597_sysctl(const char *name, const char *val, char *old,
		       size_t oldsz)
{
	char path[64];
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/sys/fs/%s", name);
	f = filp_open(path, old ? O_RDONLY : O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	if (old) {
		n = kernel_read(f, old, oldsz - 1, &pos);
		if (n >= 0)
			old[n] = '\0';
	} else {
		n = kernel_write(f, val, strlen(val), &pos);
	}
	filp_close(f, NULL);
	return n < 0 ? n : 0;
}

static void g597_remove_tree(void *unused)
{
	xfs_restore_creds();
	if (g597_old_symlinks[0])
		g597_sysctl("protected_symlinks", g597_old_symlinks, NULL, 0);
	if (g597_old_hardlinks[0])
		g597_sysctl("protected_hardlinks", g597_old_hardlinks, NULL, 0);
	xfs_umount("/proc");
	xfs_unlink(G597_SYMLINK);
	xfs_unlink(G597_HARDLINK);
	xfs_unlink(G597_TARGET);
	xfs_rmdir_settled(G597_STICKY);
	xfs_rmdir_settled(G597_ROOT);
}

/* _user_do "cat symlink": 0 if the target's contents came back */
static int g597_cat_as_other(void)
{
	char buf[sizeof(g597_msg)] = "";
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	int err;

	err = xfs_switch_creds(G597_OTHER, G597_OTHER);
	if (err)
		return err;
	f = filp_open(G597_SYMLINK, O_RDONLY, 0);
	if (IS_ERR(f)) {
		xfs_restore_creds();
		return PTR_ERR(f);
	}
	n = kernel_read(f, buf, sizeof(g597_msg) - 1, &pos);
	filp_close(f, NULL);
	xfs_restore_creds();
	if (n != sizeof(g597_msg) - 1 || memcmp(buf, g597_msg, n))
		return -EIO;
	return 0;
}

static void g597_test_symlink(struct kunit *test, const char *prot,
			      int want)
{
	KUNIT_ASSERT_EQ(test, g597_sysctl("protected_symlinks", prot, NULL, 0),
			0);
	KUNIT_ASSERT_EQ(test, xfs_symlink(G597_TARGET, G597_SYMLINK), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G597_STICKY, G597_OTHER, G597_OTHER),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G597_TARGET, G597_OWNER, G597_OWNER),
			0);
	KUNIT_EXPECT_EQ_MSG(test, g597_cat_as_other(), want,
			    "protected_symlinks = %s", prot);
	KUNIT_ASSERT_EQ(test, xfs_unlink(G597_SYMLINK), 0);
}

static void g597_test_hardlink(struct kunit *test, const char *prot,
			       int want)
{
	int err;

	KUNIT_ASSERT_EQ(test, g597_sysctl("protected_hardlinks", prot, NULL, 0),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G597_TARGET, G597_OWNER, G597_OWNER),
			0);
	/* chmod go-rw on the 0644 target */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G597_TARGET, 0600), 0);
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(G597_OTHER, G597_OTHER), 0);
	err = xfs_link(G597_TARGET, G597_HARDLINK);
	xfs_restore_creds();
	KUNIT_EXPECT_EQ_MSG(test, err, want, "protected_hardlinks = %s", prot);
	KUNIT_EXPECT_EQ(test, xfs_exists(G597_HARDLINK), !want);
	xfs_unlink(G597_HARDLINK);
}

static void protected_symlinks_and_hardlinks(struct kunit *test)
{
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	err = xfs_mkdir("/proc");
	KUNIT_ASSERT_TRUE_MSG(test, !err || err == -EEXIST, "mkdir /proc: %d",
			      err);
	KUNIT_ASSERT_EQ(test, xfs_mount_at("proc", "/proc", "proc", NULL), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g597_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, g597_sysctl("protected_symlinks", NULL,
					  g597_old_symlinks,
					  sizeof(g597_old_symlinks)), 0);
	KUNIT_ASSERT_EQ(test, g597_sysctl("protected_hardlinks", NULL,
					  g597_old_hardlinks,
					  sizeof(g597_old_hardlinks)), 0);

	/* setup_tree */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G597_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G597_STICKY), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G597_STICKY, 01777), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G597_TARGET, g597_msg,
					   sizeof(g597_msg) - 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G597_TARGET, 0644), 0);

	g597_test_symlink(test, "0", 0);
	g597_test_symlink(test, "1", -EACCES);

	g597_test_hardlink(test, "0", 0);
	g597_test_hardlink(test, "1", -EPERM);
}

static int g597_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g597_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g597_cases[] = {
	KUNIT_CASE(protected_symlinks_and_hardlinks),
	{}
};

static struct kunit_suite g597_suite = {
	.name		= "xfstests/generic/597",
	.suite_init	= g597_suite_init,
	.suite_exit	= g597_suite_exit,
	.test_cases	= g597_cases,
};

kunit_test_suites(&g597_suite);

MODULE_DESCRIPTION("xfstests generic/597 over a loopback NFS mount");
MODULE_LICENSE("GPL");
