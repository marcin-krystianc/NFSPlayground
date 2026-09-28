// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/598 over a loopback NFS mount: fs.protected_regular and
 * fs.protected_fifos.
 *
 * Upstream builds a sticky directory owned by fsgqa and, inside it, a
 * regular file and a fifo owned by fsgqa2 and readable and writable by
 * everyone. Then as qa_user (fsgqa) it opens each with O_CREAT ("xfs_io
 * -c 'open -f'"), once with the directory world-writable (1777) and once
 * only group-writable (1775), for each sysctl value 0, 1 and 2. With 1,
 * an O_CREAT open of an existing file owned by neither the caller nor the
 * directory owner is EACCES in a world-writable sticky directory; with 2,
 * also in a group-writable one (may_create_in_sticky()).
 *
 * Over NFS the owners the check compares are the ones the client got
 * from the server; the fifo is created with MKNOD and opened locally.
 *
 * Deviations: fsgqa is uid/gid 1000 and fsgqa2 1001, switched to with
 * capabilities dropped (xfs_switch_creds()). The sysctls are written
 * through /proc/sys, with procfs mounted for the test.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/string.h>

#include "xfstests_nfs_fixture.h"

#define G598_ROOT	XFS_MNT "/g598"
#define G598_STICKY	G598_ROOT "/sticky_dir"

#define G598_USER1	1001	/* fsgqa2: owns the file and the fifo */
#define G598_USER2	1000	/* fsgqa: owns the directory, qa_user */

static char g598_old_regular[16], g598_old_fifos[16];

/* sysctl -n / sysctl -w on /proc/sys/fs/<name> */
static int g598_sysctl(const char *name, const char *val, char *old,
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

static void g598_remove_tree(void *unused)
{
	xfs_restore_creds();
	if (g598_old_regular[0])
		g598_sysctl("protected_regular", g598_old_regular, NULL, 0);
	if (g598_old_fifos[0])
		g598_sysctl("protected_fifos", g598_old_fifos, NULL, 0);
	xfs_umount("/proc");
	xfs_unlink(G598_STICKY "/file");
	xfs_unlink(G598_STICKY "/fifo");
	xfs_rmdir_settled(G598_STICKY);
	xfs_rmdir_settled(G598_ROOT);
}

/* _user_do "xfs_io -c 'open -f $path'" */
static int g598_open_as_user2(const char *path)
{
	struct file *f;
	int err;

	err = xfs_switch_creds(G598_USER2, G598_USER2);
	if (err)
		return err;
	f = filp_open(path, O_RDWR | O_CREAT, 0600);
	xfs_restore_creds();
	if (IS_ERR(f))
		return PTR_ERR(f);
	filp_close(f, NULL);
	return 0;
}

/* test_access for one sysctl value; want_* is the expected open result */
static void g598_test_access(struct kunit *test, const char *sysctl,
			     const char *name, const char *prot,
			     int want_world, int want_group)
{
	char path[64];

	snprintf(path, sizeof(path), G598_STICKY "/%s", name);
	KUNIT_ASSERT_EQ(test, g598_sysctl(sysctl, prot, NULL, 0), 0);

	/* group & world writable dir */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G598_STICKY, 01777), 0);
	KUNIT_EXPECT_EQ_MSG(test, g598_open_as_user2(path), want_world,
			    "%s = %s, %s, group & world writable dir", sysctl,
			    prot, name);
	/* only group writable dir */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G598_STICKY, 01775), 0);
	KUNIT_EXPECT_EQ_MSG(test, g598_open_as_user2(path), want_group,
			    "%s = %s, %s, only group writable dir", sysctl,
			    prot, name);
}

static void protected_regular_and_fifos(struct kunit *test)
{
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	err = xfs_mkdir("/proc");
	KUNIT_ASSERT_TRUE_MSG(test, !err || err == -EEXIST, "mkdir /proc: %d",
			      err);
	KUNIT_ASSERT_EQ(test, xfs_mount_at("proc", "/proc", "proc", NULL), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g598_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, g598_sysctl("protected_regular", NULL,
					  g598_old_regular,
					  sizeof(g598_old_regular)), 0);
	KUNIT_ASSERT_EQ(test, g598_sysctl("protected_fifos", NULL,
					  g598_old_fifos,
					  sizeof(g598_old_fifos)), 0);

	/* setup_tree */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G598_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G598_STICKY), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G598_STICKY, 01777), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G598_STICKY, G598_USER2, G598_USER2),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G598_STICKY "/file", "", 0),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G598_STICKY "/file", G598_USER1,
					G598_USER1), 0);
	/* chmod o+rw */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G598_STICKY "/file", 0606), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G598_STICKY "/fifo", S_IFIFO | 0644,
					0, 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G598_STICKY "/fifo", G598_USER1,
					G598_USER1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G598_STICKY "/fifo", 0646), 0);

	g598_test_access(test, "protected_regular", "file", "0", 0, 0);
	g598_test_access(test, "protected_regular", "file", "1", -EACCES, 0);
	g598_test_access(test, "protected_regular", "file", "2", -EACCES,
			 -EACCES);

	g598_test_access(test, "protected_fifos", "fifo", "0", 0, 0);
	g598_test_access(test, "protected_fifos", "fifo", "1", -EACCES, 0);
	g598_test_access(test, "protected_fifos", "fifo", "2", -EACCES,
			 -EACCES);
}

static int g598_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g598_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g598_cases[] = {
	KUNIT_CASE(protected_regular_and_fifos),
	{}
};

static struct kunit_suite g598_suite = {
	.name		= "xfstests/generic/598",
	.suite_init	= g598_suite_init,
	.suite_exit	= g598_suite_exit,
	.test_cases	= g598_cases,
};

kunit_test_suites(&g598_suite);

MODULE_DESCRIPTION("xfstests generic/598 over a loopback NFS mount");
MODULE_LICENSE("GPL");
