// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/141 over a loopback NFS mount: a mapping that starts
 * part-way into the file.
 *
 * Upstream ("Test for xfs_io mmap read problem") writes 1024k, maps the
 * 64k window at offset 64k, and reads it back in reverse ("mread -r").
 * The bug it was written for was in xfs_io itself; what survives as a
 * filesystem test is that a mapping whose file offset is not zero reads
 * the bytes belonging to that offset.
 *
 * Over NFS that is nfs_readpage/readahead filling the mapped pages from
 * the server via nfs_vm_page_mkwrite()'s read side, with a non-zero
 * vm_pgoff -- an offset the client has to get right in both the page index
 * and the READ it issues. The port patterns the file so that every byte
 * identifies its own offset, so a mapping that reads the wrong part of the
 * file cannot pass.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G141_ROOT	XFS_MNT "/g141"
#define G141_FILE	G141_ROOT "/mmap"

#define G141_SIZE	(1024 * 1024)
#define G141_MAPOFF	(64 * 1024)
#define G141_MAPLEN	(64 * 1024)
#define G141_CHUNK	4096

static void g141_remove_tree(void *unused)
{
	xfs_unlink(G141_FILE);
	xfs_rmdir_settled(G141_ROOT);
}

/* every byte says which offset it came from */
static u8 g141_byte(loff_t off)
{
	return (u8)(off / G141_CHUNK) ^ (u8)off;
}

static void a_mapping_at_a_nonzero_offset_reads_its_own_bytes(struct kunit *test)
{
	unsigned long addr, left;
	struct file *f;
	loff_t pos = 0;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G141_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g141_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G141_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	f = filp_open(G141_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));

	while (pos < G141_SIZE) {
		loff_t base = pos;

		for (i = 0; i < G141_CHUNK; i++)
			buf[i] = g141_byte(base + i);
		KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G141_CHUNK, &pos),
				(ssize_t)G141_CHUNK);
	}
	KUNIT_ASSERT_EQ(test, vfs_fsync(f, 0), 0);

	addr = kunit_vm_mmap(test, f, 0, G141_MAPLEN, PROT_READ, MAP_SHARED,
			     G141_MAPOFF);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "kunit_vm_mmap failed");

	/* upstream's "mread -r": back to front, a chunk at a time */
	for (pos = G141_MAPLEN - G141_CHUNK; pos >= 0; pos -= G141_CHUNK) {
		left = copy_from_user(buf, (void __user *)(addr + pos),
				      G141_CHUNK);
		KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
				    "mapped read at %lld left %lu bytes",
				    pos, left);
		for (i = 0; i < G141_CHUNK; i++) {
			u8 want = g141_byte(G141_MAPOFF + pos + i);

			if (buf[i] != want) {
				KUNIT_FAIL(test,
					   "mapped byte at file offset %lld is %02x, expected %02x",
					   G141_MAPOFF + pos + i, buf[i], want);
				goto out;
			}
		}
	}
out:
	KUNIT_EXPECT_EQ(test, vm_munmap(addr, G141_MAPLEN), 0);
	filp_close(f, NULL);
}

static int g141_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g141_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g141_cases[] = {
	KUNIT_CASE(a_mapping_at_a_nonzero_offset_reads_its_own_bytes),
	{}
};

static struct kunit_suite g141_suite = {
	.name		= "xfstests/generic/141",
	.suite_init	= g141_suite_init,
	.suite_exit	= g141_suite_exit,
	.test_cases	= g141_cases,
};

kunit_test_suites(&g141_suite);

MODULE_DESCRIPTION("xfstests generic/141 over a loopback NFS mount");
MODULE_LICENSE("GPL");
