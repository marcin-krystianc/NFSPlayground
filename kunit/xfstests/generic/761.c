// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/761 over a loopback NFS mount: O_DIRECT writes from a
 * buffer another thread is changing.
 *
 * src/dio-writeback-race opens a file O_DIRECT and, for every block of a
 * 64 MiB file, fills a block-sized buffer with 0xff and starts two
 * threads: one write()s the buffer, the other memsets it to 0x00. Every
 * write must complete in full; then upstream reads the whole file back
 * with cat. The btrfs bug was a checksum computed over bytes that changed
 * before they reached the disk; the block size is _get_block_size's
 * "stat -f -c %S", the filesystem's f_frsize.
 *
 * Over NFS the write is nfs_file_direct_write() sending WRITE RPCs from
 * the pinned buffer pages while they change. The server stores whatever
 * bytes arrived, so each byte of the file must be 0xff or 0x00.
 *
 * Deviations: the buffer is kmalloc'd and written with xfs_direct_write();
 * the modify thread is one kthread signalled once per block rather than a
 * new thread per block. The export is 128 MiB (xfstests_nfs_export_opts()),
 * so the 64 MiB file fits. The read-back also checks each byte is 0xff or
 * 0x00, which upstream's cat does not.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G761_ROOT	XFS_MNT "/g761"
#define G761_FILE	G761_ROOT "/foobar"
#define G761_SIZE	(64 * 1024 * 1024)
#define G761_READ	(1024 * 1024)

struct g761_modifier {
	u8			*buf;
	size_t			blocksize;
	bool			stop;
	struct completion	go, done;
};

/* dio-writeback-race's do_modify(), once per signal */
static int g761_modify(void *arg)
{
	struct g761_modifier *m = arg;

	for (;;) {
		wait_for_completion(&m->go);
		if (READ_ONCE(m->stop))
			break;
		memset(m->buf, 0x00, m->blocksize);
		complete(&m->done);
	}
	complete(&m->done);
	return 0;
}

static void g761_remove_tree(void *unused)
{
	xfs_unlink(G761_FILE);
	xfs_rmdir_settled(G761_ROOT);
}

static void dio_write_from_a_changing_buffer(struct kunit *test)
{
	struct g761_modifier m = {};
	struct task_struct *t;
	struct kstatfs sfs;
	loff_t filepos, off;
	struct file *f;
	ssize_t n;
	size_t i;
	u8 *rbuf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G761_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g761_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_statfs(XFS_MNT, &sfs), 0);
	m.blocksize = sfs.f_frsize;
	kunit_info(test, "blocksize=%zu filesize=%d\n", m.blocksize,
		   G761_SIZE);
	KUNIT_ASSERT_GT(test, m.blocksize, (size_t)0);
	m.buf = kunit_kmalloc(test, m.blocksize, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, m.buf);

	f = filp_open(G761_FILE, O_DIRECT | O_WRONLY | O_CREAT, 0600);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	init_completion(&m.go);
	init_completion(&m.done);
	t = kthread_run(g761_modify, &m, "g761-modify");
	if (IS_ERR(t)) {
		filp_close(f, NULL);
		KUNIT_FAIL_AND_ABORT(test, "kthread_run: %ld", PTR_ERR(t));
	}

	for (filepos = 0; filepos < G761_SIZE; filepos += m.blocksize) {
		off = filepos;
		memset(m.buf, 0xff, m.blocksize);
		complete(&m.go);
		n = xfs_direct_write(f, m.buf, m.blocksize, &off);
		wait_for_completion(&m.done);
		if (n != m.blocksize) {
			KUNIT_FAIL(test, "write at %lld returned %zd", filepos,
				   n);
			break;
		}
	}
	WRITE_ONCE(m.stop, true);
	complete(&m.go);
	wait_for_completion(&m.done);
	filp_close(f, NULL);

	/* cat $SCRATCH_MNT/foobar > /dev/null */
	rbuf = kunit_kmalloc(test, G761_READ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rbuf);
	for (off = 0; off < G761_SIZE; off += G761_READ) {
		n = xfs_read_range(G761_FILE, rbuf, G761_READ, off);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G761_READ,
				    "read at %lld returned %zd", off, n);
		for (i = 0; i < G761_READ; i++)
			if (rbuf[i] != 0xff && rbuf[i] != 0x00) {
				KUNIT_FAIL(test, "byte %lld is %02x",
					   off + i, rbuf[i]);
				return;
			}
	}
}

static int g761_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=134217728,nr_inodes=32768");
	return xfstests_nfs_get();
}

static void g761_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g761_cases[] = {
	KUNIT_CASE_SLOW(dio_write_from_a_changing_buffer),
	{}
};

static struct kunit_suite g761_suite = {
	.name		= "xfstests/generic/761",
	.suite_init	= g761_suite_init,
	.suite_exit	= g761_suite_exit,
	.test_cases	= g761_cases,
};

kunit_test_suites(&g761_suite);

MODULE_DESCRIPTION("xfstests generic/761 over a loopback NFS mount");
MODULE_LICENSE("GPL");
