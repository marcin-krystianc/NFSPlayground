// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/647 over a loopback NFS mount: page faults on a mapping of the file being read or written.
 *
 * Upstream runs src/mmap-rw-fault: five times over, a two-page file whose
 * first page is a hole and whose second holds a known byte is mapped
 * MAP_PRIVATE, and one I/O is done whose user buffer is in that mapping:
 *
 *	pread  of page 1 into mapped page 0, buffered and with O_DIRECT
 *	pwrite of mapped page 1 to offset 0, buffered and with O_DIRECT
 *	pread  of the hole at offset 0 into mapped page 0, O_DIRECT
 *
 * Each makes the kernel fault a page of the same file in the middle of
 * an I/O on that file, and each must move a whole page and leave mapped
 * page 0 holding what was read or written. The direct cases are the
 * sharp ones: a filesystem that faults the source in and then retries
 * with page faults disabled can livelock. generic/729 runs the same
 * program with -2, which adds a sixth case.
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

#define G647_DIR	"g647"

static void g647_remove_tree(void *unused)
{
	xfs_unlink(XFS_MNT "/" G647_DIR "/mmap-rw-fault.tmp");
	xfs_rmdir_settled(XFS_MNT "/" G647_DIR);
}

static void faults_on_the_files_own_mapping_are_handled(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(XFS_MNT "/" G647_DIR), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g647_remove_tree,
						  NULL), 0);
	xfs_mmap_rw_fault(test, G647_DIR "/mmap-rw-fault.tmp", false);
}

static int g647_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g647_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g647_cases[] = {
	KUNIT_CASE(faults_on_the_files_own_mapping_are_handled),
	{}
};

static struct kunit_suite g647_suite = {
	.name		= "xfstests/generic/647",
	.suite_init	= g647_suite_init,
	.suite_exit	= g647_suite_exit,
	.test_cases	= g647_cases,
};

kunit_test_suites(&g647_suite);

MODULE_DESCRIPTION("xfstests generic/647 over a loopback NFS mount");
MODULE_LICENSE("GPL");
