// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/184 over a loopback NFS mount: mknod makes working
 * nodes.
 *
 * Upstream is three lines: mknod a character device (1,3 -- /dev/null),
 * chmod it 666, echo into it, and require the whole thing to exit 0.
 *
 * Over NFSv4 the node is created on the server by a CREATE with type
 * NF4CHR carrying the rdev, and the client has to read the major/minor
 * back out of the attributes on lookup, install them with init_special_inode()
 * and open the *client's* driver -- the file is a device node stored on
 * the server, not a channel to the server. The port checks each of those
 * steps, which upstream's exit status only covers implicitly: the mode and
 * rdev that come back from the server, and that a write to the node is
 * absorbed by the null driver rather than reaching a file.
 *
 * A fifo is added for the same reason S_IFIFO is the other interesting
 * case: it is created on the server but implemented entirely on the
 * client, and its size must stay 0.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G184_ROOT	XFS_MNT "/g184"
#define G184_NULL	G184_ROOT "/null"
#define G184_FIFO	G184_ROOT "/fifo"

static void g184_remove_tree(void *unused)
{
	xfs_unlink(G184_FIFO);
	xfs_unlink(G184_NULL);
	xfs_rmdir_settled(G184_ROOT);
}

static void a_character_node_is_created_and_works(struct kunit *test)
{
	static const char msg[] = "fred\n";
	unsigned long addr;
	struct kstat st;
	struct file *f;
	loff_t pos = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G184_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g184_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mknod(G184_NULL, S_IFCHR | 0600, 1, 3), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G184_NULL, 0666), 0);

	/* the attributes the client read back from the server */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G184_NULL, &st), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISCHR(st.mode),
			      "mode %o is not a character device", st.mode);
	KUNIT_EXPECT_EQ(test, st.mode & 07777, 0666);
	KUNIT_EXPECT_EQ_MSG(test, MAJOR(st.rdev), 1U, "major is %u",
			    MAJOR(st.rdev));
	KUNIT_EXPECT_EQ_MSG(test, MINOR(st.rdev), 3U, "minor is %u",
			    MINOR(st.rdev));

	/*
	 * upstream's "echo fred > $TEST_DIR/null".
	 *
	 * The bytes have to come from a user address: kernel_write() refuses
	 * any file whose fops wire up both ->write and ->write_iter
	 * ("implies very convoluted semantics", fs/read_write.c), and
	 * null_fops wires both. write(2)'s own body has no such rule, so
	 * this is the syscall, with the string in an anonymous mapping.
	 */
	addr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "anonymous mapping failed");
	KUNIT_ASSERT_EQ(test,
			copy_to_user((void __user *)addr, msg, strlen(msg)),
			0UL);

	f = filp_open(G184_NULL, O_WRONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_EXPECT_EQ(test,
			vfs_write(f, (const char __user *)addr, strlen(msg),
				  &pos),
			(ssize_t)strlen(msg));
	filp_close(f, NULL);
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, PAGE_SIZE), 0);

	/* it went to the null driver, not into a file on the server */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G184_NULL, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
			    "the device node grew to %lld bytes", st.size);
}

static void a_fifo_is_created_with_the_right_type(struct kunit *test)
{
	struct kstat st;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G184_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g184_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mknod(G184_FIFO, S_IFIFO | 0644, 0, 0), 0);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G184_FIFO, &st), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISFIFO(st.mode),
			      "mode %o is not a fifo", st.mode);
	KUNIT_EXPECT_EQ(test, st.size, 0LL);
}

static int g184_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g184_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g184_cases[] = {
	KUNIT_CASE(a_character_node_is_created_and_works),
	KUNIT_CASE(a_fifo_is_created_with_the_right_type),
	{}
};

static struct kunit_suite g184_suite = {
	.name		= "xfstests/generic/184",
	.suite_init	= g184_suite_init,
	.suite_exit	= g184_suite_exit,
	.test_cases	= g184_cases,
};

kunit_test_suites(&g184_suite);

MODULE_DESCRIPTION("xfstests generic/184 over a loopback NFS mount");
MODULE_LICENSE("GPL");
