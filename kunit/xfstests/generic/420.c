// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/420 over a loopback NFS mount: punching a hole past
 * EOF with FALLOC_FL_KEEP_SIZE leaves the size alone.
 *
 * Upstream writes 2048 bytes and punches 2048..4096, a range that lies
 * wholly beyond EOF, then checks that stat still reports 2048.
 *
 * Over NFSv4.2 the punch is nfs42_fallocate() -> nfs42_proc_deallocate(),
 * a DEALLOCATE RPC; the size the client reports afterwards comes from the
 * post-op attributes that nfs_post_op_update_inode_force_wcc() applies.
 * The port checks the client's size and the server's.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>

#include "xfstests_nfs_fixture.h"

#define G420_ROOT	XFS_MNT "/g420"
#define G420_FILE	G420_ROOT "/testfile.420"
#define G420_SERVER	XFS_EXPORT "/g420/testfile.420"

static void g420_remove_tree(void *unused)
{
	xfs_unlink(G420_FILE);
	xfs_rmdir_settled(G420_ROOT);
}

static void punch_past_eof_keeps_size(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G420_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g420_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, 2048, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xcd, 2048);

	/* xfs_io -f -t -c "pwrite -b 2048 0 2048" */
	f = filp_open(G420_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, 2048, &pos), (ssize_t)2048);
	filp_close(f, NULL);

	/* xfs_io -c "fpunch 2048 2048" */
	f = filp_open(G420_FILE, O_RDWR, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "reopen: %ld", PTR_ERR(f));
	KUNIT_EXPECT_EQ(test,
			vfs_fallocate(f, FALLOC_FL_PUNCH_HOLE |
				      FALLOC_FL_KEEP_SIZE, 2048, 2048), 0);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G420_FILE, &st), 0);
	KUNIT_EXPECT_EQ(test, st.size, (loff_t)2048);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G420_SERVER, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)2048,
			    "server size is %lld", st.size);
}

static int g420_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g420_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g420_cases[] = {
	KUNIT_CASE(punch_past_eof_keeps_size),
	{}
};

static struct kunit_suite g420_suite = {
	.name		= "xfstests/generic/420",
	.suite_init	= g420_suite_init,
	.suite_exit	= g420_suite_exit,
	.test_cases	= g420_cases,
};

kunit_test_suites(&g420_suite);

MODULE_DESCRIPTION("xfstests generic/420 over a loopback NFS mount");
MODULE_LICENSE("GPL");
