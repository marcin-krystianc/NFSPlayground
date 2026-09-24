// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/729 over a loopback NFS mount: the same, plus a direct write of a mapped page onto itself.
 *
 * Upstream runs src/mmap-rw-fault -2: generic/647's five cases (reads
 * and writes, buffered and O_DIRECT, whose user buffer is a MAP_PRIVATE
 * mapping of the file being read or written), plus a sixth: an O_DIRECT
 * pwrite of mapped page 1 back onto the file offset it maps. Upstream's
 * own comment says why that is the dangerous one -- "the kernel will
 * invalidate the page cache before carrying out the write, so filesystems
 * that fault in the page and then carry out the direct I/O write with
 * page faults disabled will never make any progress". A livelock would
 * hang the case, and KUnit's timeout is what would report it.
 *
 * Over NFS the fault is serviced by nfs_read_folio() (the mapping is
 * private, so a write fault copies) against the same inode the I/O holds,
 * and the direct path is nfs_direct_read()/nfs_direct_write() extracting
 * pages from a mapping of that same file.
 *
 * xfs_mmap_rw_fault() is mmap-rw-fault: the buffer is a user address in a
 * kunit_vm_mmap() mapping and each I/O is pread/pwrite's iterator, with
 * O_DIRECT per case.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/fs.h>

#include "xfstests_nfs_fixture.h"

#define G729_DIR	"g729"

static void g729_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G729_DIR "/mmap-rw-fault.tmp");
	xfs_rmdir_settled(XFS_MNT "/" G729_DIR);
}

static void mmap_rw_fault_with_a_direct_write_onto_itself(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G729_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g729_remove_tree,
						  NULL), 0);
	xfs_mmap_rw_fault(test, G729_DIR "/mmap-rw-fault.tmp", true);
}

static int g729_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g729_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g729_cases[] = {
	KUNIT_CASE(mmap_rw_fault_with_a_direct_write_onto_itself),
	{}
};

static struct kunit_suite g729_suite = {
	.name		= "xfstests/generic/729",
	.suite_init	= g729_suite_init,
	.suite_exit	= g729_suite_exit,
	.test_cases	= g729_cases,
};

kunit_test_suites(&g729_suite);

MODULE_DESCRIPTION("xfstests generic/729 over a loopback NFS mount");
MODULE_LICENSE("GPL");
