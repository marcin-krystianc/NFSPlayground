// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/024 over a loopback NFS mount: renameat2 flags.
 *
 * Upstream 024/025/078 exercise RENAME_NOREPLACE, RENAME_EXCHANGE and
 * RENAME_WHITEOUT; each _notruns where the filesystem lacks the flag.
 * The NFS picture is two-layered, and this port pins both layers:
 *
 *  - RENAME_NOREPLACE against an existing target fails with EEXIST from
 *    the VFS itself (the exclusive target lookup), so the no-replace
 *    guarantee works generically over NFS without any protocol support;
 *  - once past the VFS (absent target), every flag reaches nfs_rename(),
 *    which rejects all of them with EINVAL -- NFSv4 has no flagged
 *    RENAME, which is why EXCHANGE and WHITEOUT can never work.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include "internal.h"

#include "xfstests_nfs_fixture.h"

#define G024_ROOT	XFS_MNT "/g024"

static void g024_remove_tree(void *unused)
{
	xfs_unlink(G024_ROOT "/a");
	xfs_unlink(G024_ROOT "/b");
	xfs_rmdir(G024_ROOT);
}

static int g024_rename_flags(const char *a, const char *b, unsigned int fl)
{
	return do_renameat2(AT_FDCWD, getname_kernel(a),
			    AT_FDCWD, getname_kernel(b), fl);
}

static void every_renameat2_flag_is_rejected_with_einval(struct kunit *test)
{
	static const struct { const char *name; unsigned int fl; } flags[] = {
		{ "RENAME_NOREPLACE",	RENAME_NOREPLACE },
		{ "RENAME_EXCHANGE",	RENAME_EXCHANGE },
		{ "RENAME_WHITEOUT",	RENAME_WHITEOUT },
	};
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G024_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g024_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G024_ROOT "/a", "A", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G024_ROOT "/b", "B", 1), 0);

	/*
	 * NOREPLACE with an existing target: the VFS's exclusive lookup
	 * refuses with EEXIST before NFS is consulted -- the guarantee
	 * holds generically.
	 */
	KUNIT_EXPECT_EQ_MSG(test,
			    g024_rename_flags(G024_ROOT "/a", G024_ROOT "/b",
					      RENAME_NOREPLACE),
			    -EEXIST,
			    "NOREPLACE onto an existing target must be VFS-level EEXIST");

	/* with no target in the way, the flag reaches nfs_rename: EINVAL */
	KUNIT_EXPECT_EQ_MSG(test,
			    g024_rename_flags(G024_ROOT "/a", G024_ROOT "/c",
					      RENAME_NOREPLACE),
			    -EINVAL,
			    "NOREPLACE past the VFS must be rejected by the NFS client");

	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		int err;

		if (flags[i].fl == RENAME_NOREPLACE)
			continue;	/* covered above, both layers */
		err = g024_rename_flags(G024_ROOT "/a", G024_ROOT "/b",
					flags[i].fl);
		KUNIT_EXPECT_EQ_MSG(test, err, -EINVAL,
				    "%s over NFS: expected EINVAL, got %d",
				    flags[i].name, err);
	}

	/* both names untouched, contents intact */
	KUNIT_EXPECT_TRUE(test, xfs_exists(G024_ROOT "/a"));
	KUNIT_EXPECT_TRUE(test, xfs_exists(G024_ROOT "/b"));
	{
		char c;

		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G024_ROOT "/a", &c, 1, 0),
				(ssize_t)1);
		KUNIT_EXPECT_EQ(test, c, (char)0x41);
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(G024_ROOT "/b", &c, 1, 0),
				(ssize_t)1);
		KUNIT_EXPECT_EQ(test, c, (char)0x42);
	}
}

static int g024_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g024_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g024_cases[] = {
	KUNIT_CASE(every_renameat2_flag_is_rejected_with_einval),
	{}
};

static struct kunit_suite g024_suite = {
	.name		= "xfstests/generic/024",
	.suite_init	= g024_suite_init,
	.suite_exit	= g024_suite_exit,
	.test_cases	= g024_cases,
};

kunit_test_suites(&g024_suite);

MODULE_DESCRIPTION("xfstests generic/024 over a loopback NFS mount");
MODULE_LICENSE("GPL");
