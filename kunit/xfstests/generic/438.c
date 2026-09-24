// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/438 over a loopback NFS mount: growing a file one byte
 * at a time through fallocate while writing to it through a mapping, with
 * writeback forced underneath.
 *
 * src/t_mmap_fallocate maps a file, then for every byte in turn extends
 * the file by that one byte with fallocate(2) and writes 0x78 into it
 * through the mapping, checking the byte reads back immediately; at the
 * end it re-checks the whole file. The shell test runs a loop of fsyncs
 * beside it, because the bug it was written for -- "too much is zeroed in
 * the tail page that gets written out just while the file gets extended"
 * -- needs writeback to happen in the middle.
 *
 * Over NFS the tail page is where nfs_update_folio()'s dirty-range
 * bookkeeping matters: the page is partially valid, an ALLOCATE has just
 * moved EOF, and a COMMIT may be writing the page back at the same
 * moment. Losing the range means the mapped byte reads back as zero.
 *
 * As upstream, the file first holds a newline (echo > $FILE), the loop's
 * xfs_io -c fsync opens, fsyncs and closes the file each time, and
 * t_mmap_fallocate truncates the file and maps all 256 KiB before the
 * first byte exists. The fsync loop is a kthread.
 *
 * Not in upstream: the server's copy must hold the same bytes at the end.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>

#include "xfstests_nfs_fixture.h"

#define G438_ROOT	XFS_MNT "/g438"
#define G438_FILE	G438_ROOT "/testfile_fallocate"
#define G438_SERVER	XFS_EXPORT "/g438/testfile_fallocate"
#define G438_SIZE	(256 * 1024)	/* t_mmap_fallocate $FILE 256 */
#define G438_BYTE	0x78

struct g438_syncer {
	atomic_t		stop;
	unsigned long		syncs;
	int			err;
	struct completion	done;
};

static int g438_fsync_loop(void *arg)
{
	struct g438_syncer *s = arg;

	/* while [ $STOP -eq 0 ]; do xfs_io -c fsync $FILE; done */
	while (!atomic_read(&s->stop)) {
		struct file *f = filp_open(G438_FILE, O_RDWR, 0);
		int err = IS_ERR(f) ? PTR_ERR(f) : vfs_fsync(f, 0);

		if (!IS_ERR(f))
			filp_close(f, NULL);
		if (err) {
			s->err = err;
			break;
		}
		s->syncs++;
		cond_resched();
	}
	complete(&s->done);
	return 0;
}

static void g438_remove_tree(void *unused)
{
	xfs_settle_fput();
	xfs_unlink(G438_FILE);
	xfs_rmdir_settled(G438_ROOT);
}

static void bytes_written_while_the_file_grows_survive(struct kunit *test)
{
	struct g438_syncer s = {};
	struct task_struct *t;
	unsigned long addr;
	struct file *f;
	u8 *got;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G438_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g438_remove_tree, NULL),
			0);

	/* echo > $FILE */
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G438_FILE, "\n", 1), 0);

	init_completion(&s.done);
	atomic_set(&s.stop, 0);
	t = kthread_run(g438_fsync_loop, &s, "g438-fsync");
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
			       PTR_ERR(t));

	/* t_mmap_fallocate $FILE 256 */
	f = filp_open(G438_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	addr = kunit_vm_mmap(test, f, 0, G438_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	for (i = 0; i < G438_SIZE; i++) {
		u8 v = G438_BYTE, back = 0;

		KUNIT_ASSERT_EQ_MSG(test, vfs_fallocate(f, 0, i, 1), 0,
				    "extending to byte %d failed", i);
		KUNIT_ASSERT_EQ_MSG(test,
				    copy_to_user((void __user *)(addr + i), &v,
						 1),
				    0UL, "writing byte %d failed", i);
		KUNIT_ASSERT_EQ(test,
				copy_from_user(&back,
					       (void __user *)(addr + i), 1),
				0UL);
		KUNIT_ASSERT_EQ_MSG(test, back, G438_BYTE,
				    "byte %d did not read back as written", i);
	}

	atomic_set(&s.stop, 1);
	wait_for_completion(&s.done);
	KUNIT_EXPECT_EQ_MSG(test, s.err, 0, "the fsync loop failed: %d",
			    s.err);
	KUNIT_EXPECT_GT_MSG(test, s.syncs, 0UL, "the fsync loop never ran");

	/* upstream's final pass over the whole file */
	got = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);
	for (i = 0; i < G438_SIZE; i++) {
		if (i % PAGE_SIZE == 0)
			KUNIT_ASSERT_EQ(test,
					copy_from_user(got,
						       (void __user *)(addr + i),
						       PAGE_SIZE), 0UL);
		if (got[i % PAGE_SIZE] != G438_BYTE) {
			KUNIT_FAIL(test, "byte %d was modified: %02x", i,
				   got[i % PAGE_SIZE]);
			break;
		}
	}

	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G438_SIZE), 0);
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	filp_close(f, NULL);

	/* not upstream: the same bytes are on the server */
	for (i = 0; i < G438_SIZE; i++) {
		if (i % PAGE_SIZE == 0)
			KUNIT_ASSERT_EQ(test,
					xfs_read_range(G438_SERVER, got,
						       PAGE_SIZE, i),
					(ssize_t)PAGE_SIZE);
		if (got[i % PAGE_SIZE] != G438_BYTE) {
			KUNIT_FAIL(test, "server byte %d is %02x", i,
				   got[i % PAGE_SIZE]);
			break;
		}
	}
}

static int g438_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g438_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g438_cases[] = {
	KUNIT_CASE_SLOW(bytes_written_while_the_file_grows_survive),
	{}
};

static struct kunit_suite g438_suite = {
	.name		= "xfstests/generic/438",
	.suite_init	= g438_suite_init,
	.suite_exit	= g438_suite_exit,
	.test_cases	= g438_cases,
};

kunit_test_suites(&g438_suite);

MODULE_DESCRIPTION("xfstests generic/438 over a loopback NFS mount");
MODULE_LICENSE("GPL");
