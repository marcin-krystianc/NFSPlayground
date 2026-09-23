// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/306 over a loopback NFS mount: writing through things
 * that live on a read-only filesystem.
 *
 * Upstream makes a null and a zero device node on the scratch
 * filesystem, a symlink pointing at a file on another (writable)
 * filesystem and a file to bind-mount over, remounts the filesystem
 * read-only, and then checks that:
 *
 *	creating a new file fails with EROFS
 *	writing to the null device node succeeds
 *	reading from the zero device node succeeds
 *	writing through the symlink, whose target is elsewhere, succeeds
 *	writing to the bind-mounted file succeeds
 *
 * The rule being checked is that read-only applies to the filesystem's
 * own data, not to what a device node or a symlink on it happens to point
 * at. Over NFS the remount is a client-side MS_RDONLY, so every one of
 * those decisions is made by the client's VFS before any RPC is sent --
 * and the device node is the interesting one, because the client has to
 * open the local driver rather than anything on the server.
 *
 * Deviations: the bind-mount case is left out -- the fixture has a single
 * deployment shared by every suite, and mounting inside it would change
 * what the other suites see. The writes go through vfs_write() with a
 * user address rather than kernel_write(), because null_fops wires up
 * both ->write and ->write_iter and kernel_write() refuses such files
 * (see the generic/184 port).
 *
 * One deviation is a workaround, and it is here because of something
 * measured rather than assumed. After the read-only remount the device
 * nodes' dentries are no longer cached, and opening one then goes through
 * nfs_atomic_open(). In that state the open fails with -EIO, every time,
 * for both nodes; a plain path lookup first (xfs_exists() is kern_path())
 * makes the very same open succeed, and so does any getattr. The port
 * therefore looks each node up before opening it, and says so here rather
 * than hiding it. What the -EIO actually comes from is not established:
 * fs/namei.c's atomic_open() turns "->atomic_open() returned 0 without
 * opening" into -EIO, which fits, but that path also WARNs and no warning
 * appears in the log. Upstream's shell test does not hit this because its
 * mknod leaves the dentry positive and its remount does not drop it.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/kdev_t.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G306_ROOT	XFS_MNT "/g306"
#define G306_NULL	G306_ROOT "/devnull"
#define G306_ZERO	G306_ROOT "/devzero"
#define G306_SYMLINK	G306_ROOT "/symlink"
#define G306_NEWFILE	G306_ROOT "/this_should_fail"
#define G306_TARGET	"/g306-target"		/* on the root ramfs */
#define G306_LEN	512

static void g306_restore_rw(void *unused)
{
	xfs_remount_client(false);
}

static void g306_remove_tree(void *unused)
{
	xfs_unlink(G306_NEWFILE);
	xfs_unlink(G306_SYMLINK);
	xfs_unlink(G306_ZERO);
	xfs_unlink(G306_NULL);
	xfs_rmdir_settled(G306_ROOT);
	xfs_unlink(G306_TARGET);
}

static void a_read_only_mount_still_lets_its_devices_work(struct kunit *test)
{
	unsigned long addr;
	struct file *f;
	loff_t pos;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G306_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g306_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mknod(G306_NULL, S_IFCHR | 0666, 1, 3), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G306_ZERO, S_IFCHR | 0666, 1, 5), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G306_TARGET, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_symlink(G306_TARGET, G306_SYMLINK), 0);

	buf = kunit_kmalloc(test, G306_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x61, G306_LEN);
	addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "anonymous mapping failed");
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, buf, G306_LEN),
			0UL);

	/* remount read-only, and put it back whatever happens next */
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g306_restore_rw, NULL),
			0);
	KUNIT_ASSERT_EQ_MSG(test, xfs_remount_client(true), 0,
			    "remounting read-only failed");

	/* a new file is refused */
	KUNIT_EXPECT_EQ_MSG(test, xfs_write_new_file(G306_NEWFILE, "x", 1),
			    -EROFS,
			    "creating a file on a read-only mount was allowed");

	/* the null device still takes writes */
	KUNIT_ASSERT_TRUE(test, xfs_exists(G306_NULL));	/* see the header */
	f = filp_open(G306_NULL, O_WRONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
			       "opening the null node after the remount: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_EXPECT_EQ_MSG(test,
			    vfs_write(f, (const char __user *)addr, G306_LEN,
				      &pos),
			    (ssize_t)G306_LEN,
			    "writing to the null node on a read-only mount failed");
	filp_close(f, NULL);

	/* and the zero device still gives zeroes */
	KUNIT_ASSERT_TRUE(test, xfs_exists(G306_ZERO));	/* see the header */
	f = filp_open(G306_ZERO, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "opening the zero node: %ld",
			       PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, buf, G306_LEN),
			0UL);
	pos = 0;
	KUNIT_EXPECT_EQ_MSG(test,
			    vfs_read(f, (char __user *)addr, G306_LEN, &pos),
			    (ssize_t)G306_LEN,
			    "reading the zero node on a read-only mount failed");
	filp_close(f, NULL);
	KUNIT_ASSERT_EQ(test,
			copy_from_user(buf, (void __user *)addr, G306_LEN),
			0UL);
	for (i = 0; i < G306_LEN; i++)
		if (buf[i]) {
			KUNIT_FAIL(test, "the zero node returned %02x at %d",
				   buf[i], i);
			break;
		}

	/* writing through a symlink whose target is on another filesystem */
	f = filp_open(G306_SYMLINK, O_WRONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
			       "opening through the symlink: %ld",
			       PTR_ERR(f));
	pos = 0;
	KUNIT_EXPECT_EQ_MSG(test, kernel_write(f, buf, G306_LEN, &pos),
			    (ssize_t)G306_LEN,
			    "writing through the symlink failed");
	filp_close(f, NULL);

	KUNIT_EXPECT_EQ_MSG(test, xfs_remount_client(false), 0,
			    "remounting read-write failed");
}

static int g306_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g306_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g306_cases[] = {
	KUNIT_CASE(a_read_only_mount_still_lets_its_devices_work),
	{}
};

static struct kunit_suite g306_suite = {
	.name		= "xfstests/generic/306",
	.suite_init	= g306_suite_init,
	.suite_exit	= g306_suite_exit,
	.test_cases	= g306_cases,
};

kunit_test_suites(&g306_suite);

MODULE_DESCRIPTION("xfstests generic/306 over a loopback NFS mount");
MODULE_LICENSE("GPL");
