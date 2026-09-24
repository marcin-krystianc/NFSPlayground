// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/609 over a loopback NFS mount: an O_DIRECT | O_DSYNC
 * write.
 *
 * Upstream is one line -- "xfs_io -f -d -s -c 'pwrite 0 64k'" -- written
 * so that a filesystem whose locking is incompatible with
 * generic_write_sync() being called from the iomap direct path gets a
 * lockdep warning. The golden output is just the successful write.
 *
 * Over NFS the two flags are a single question: a direct write with
 * O_DSYNC must reach stable storage without a separate COMMIT round trip
 * being lost. nfs_direct_write() picks FLUSH_STABLE or FLUSH_COND_STABLE
 * for the WRITEs and then has to honour whichever the server granted, so
 * this port checks the outcome the shell test cannot see: after the write
 * returns, the bytes are already on the server, without the client
 * flushing anything.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G609_ROOT	XFS_MNT "/g609"
#define G609_FILE	G609_ROOT "/file"
#define G609_SERVER	XFS_EXPORT "/g609/file"
#define G609_LEN	(64 * 1024)

static void g609_remove_tree(void *unused)
{
	xfs_unlink(G609_FILE);
	xfs_rmdir_settled(G609_ROOT);
}

static void a_direct_dsync_write_is_on_the_server_when_it_returns(
		struct kunit *test)
{
	struct file *f;
	loff_t pos = 0;
	u8 *buf, *got;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G609_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g609_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G609_LEN, GFP_KERNEL);
	got = kunit_kmalloc(test, G609_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	memset(buf, 0x39, G609_LEN);

	f = filp_open(G609_FILE, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT | O_DSYNC,
		      0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ_MSG(test, xfs_direct_write(f, buf, G609_LEN, &pos),
			    (ssize_t)G609_LEN, "the write did not complete");

	/* no fsync, no close: O_DSYNC means it is already there */
	KUNIT_ASSERT_EQ(test, xfs_read_range(G609_SERVER, got, G609_LEN, 0),
			(ssize_t)G609_LEN);
	for (i = 0; i < G609_LEN; i++)
		if (got[i] != 0x39) {
			KUNIT_FAIL(test,
				   "server byte %d is %02x before any flush",
				   i, got[i]);
			break;
		}

	filp_close(f, NULL);
}

static int g609_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g609_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g609_cases[] = {
	KUNIT_CASE(a_direct_dsync_write_is_on_the_server_when_it_returns),
	{}
};

static struct kunit_suite g609_suite = {
	.name		= "xfstests/generic/609",
	.suite_init	= g609_suite_init,
	.suite_exit	= g609_suite_exit,
	.test_cases	= g609_cases,
};

kunit_test_suites(&g609_suite);

MODULE_DESCRIPTION("xfstests generic/609 over a loopback NFS mount");
MODULE_LICENSE("GPL");
