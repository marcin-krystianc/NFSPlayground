// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/754 over a loopback NFS mount: symlink targets of 33
 * to 1023 bytes read back intact after a mount cycle.
 *
 * Upstream creates symlink.32, symlink.64, ... symlink.992 on SCRATCH_MNT,
 * the Nth pointing at N copies of a 33-byte string, and on each sets and
 * removes three trusted.* xattrs with "attr -R"; the XFS bug was a short
 * remote symlink target going wrong when the attr fork changed shape.
 * Then it cycles the mount and requires readlink to return every target
 * unchanged.
 *
 * NFSv4.2 carries only user.* xattrs, so the attr calls fail; upstream
 * sends their errors to /dev/null and the test still runs. What is left
 * over NFS is SYMLINK with targets up to 1023 bytes, then READLINK of each
 * from a new superblock with an empty cache.
 *
 * Deviations: SCRATCH_MNT is the fixture's second mount of the export
 * (XFS_SCRATCH_MNT), cycled with xfs_scratch_umount()/xfs_scratch_mount().
 * The trusted.* attr calls are left out, since over NFS they cannot
 * change anything.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/string.h>

#include "xfstests_nfs_fixture.h"

#define G754_DIR	XFS_SCRATCH_MNT "/g754"
#define G754_ADD	"0123456789ABCDEF01234567890ABCDEF"
#define G754_ADDLEN	(sizeof(G754_ADD) - 1)
#define G754_MAX	(31 * G754_ADDLEN)

static void g754_remove_tree(void *unused)
{
	char path[64];
	int size;

	xfs_scratch_umount();
	for (size = 32; size < 1024; size += 32) {
		snprintf(path, sizeof(path), XFS_MNT "/g754/symlink.%d", size);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(XFS_MNT "/g754");
}

static void symlink_targets_survive_a_mount_cycle(struct kunit *test)
{
	char path[64];
	char *target, *got;
	int size, len;
	ssize_t n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g754_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G754_DIR), 0);

	target = kunit_kzalloc(test, G754_MAX + 1, GFP_KERNEL);
	got = kunit_kzalloc(test, G754_MAX + 2, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, target);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	for (size = 32, len = 0; size < 1024; size += 32) {
		memcpy(target + len, G754_ADD, G754_ADDLEN);
		len += G754_ADDLEN;
		snprintf(path, sizeof(path), G754_DIR "/symlink.%d", size);
		KUNIT_ASSERT_EQ_MSG(test, xfs_symlink(target, path), 0,
				    "ln -s to %s (%d bytes)", path, len);
	}

	/* _scratch_cycle_mount */
	KUNIT_ASSERT_EQ(test, xfs_scratch_umount(), 0);
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);

	for (size = 32, len = 0; size < 1024; size += 32) {
		len += G754_ADDLEN;
		snprintf(path, sizeof(path), G754_DIR "/symlink.%d", size);
		n = xfs_readlink(path, got, G754_MAX + 2);
		KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)len,
				    "%s: readlink returned %zd", path, n);
		if (n == len)
			KUNIT_EXPECT_MEMEQ_MSG(test, got, target, len,
					       "%s: target is corrupt", path);
	}
}

static int g754_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g754_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g754_cases[] = {
	KUNIT_CASE(symlink_targets_survive_a_mount_cycle),
	{}
};

static struct kunit_suite g754_suite = {
	.name		= "xfstests/generic/754",
	.suite_init	= g754_suite_init,
	.suite_exit	= g754_suite_exit,
	.test_cases	= g754_cases,
};

kunit_test_suites(&g754_suite);

MODULE_DESCRIPTION("xfstests generic/754 over a loopback NFS mount");
MODULE_LICENSE("GPL");
