// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/394 over a loopback NFS mount: truncate against
 * RLIMIT_FSIZE.
 *
 * Upstream sets the file size limit to 1 GiB and truncates three files:
 * to one byte below the limit, to exactly the limit, and to one byte
 * above it. The first two must succeed and the third must fail with
 * "File size limit exceeded" -- the limit is a boundary the last byte
 * may touch but not cross.
 *
 * generic/228 already covers the fallocate side of the same limit over
 * NFS; this is the truncate side, and it is a different path: the check
 * lives in inode_newsize_ok()/do_truncate() before nfs_setattr() ever
 * issues a SETATTR, so a limit violation must cost no RPC at all, and
 * the two legal truncates must still reach the server.
 *
 * Deviations: as in 228, the limit is set on the KUnit case's own signal
 * struct and restored afterwards (a kthread ignores the SIGXFSZ that
 * accompanies EFBIG, so upstream's "ulimit -c 0" has no analogue). The
 * files are sparse -- a 1 GiB truncate allocates nothing on the tmpfs
 * export.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/resource.h>

#include "xfstests_nfs_fixture.h"

#define G394_ROOT	XFS_MNT "/g394"
#define G394_BELOW	G394_ROOT "/394-1"
#define G394_AT		G394_ROOT "/394"
#define G394_ABOVE	G394_ROOT "/394+1"

#define G394_LIMIT	(1024ULL * 1024 * 1024)

static void g394_remove_tree(void *unused)
{
	xfs_unlink(G394_ABOVE);
	xfs_unlink(G394_AT);
	xfs_unlink(G394_BELOW);
	xfs_rmdir_settled(G394_ROOT);
}

static void g394_restore_rlimit(void *arg)
{
	struct rlimit *saved = arg;

	current->signal->rlim[RLIMIT_FSIZE] = *saved;
}

static void truncate_stops_at_the_file_size_limit(struct kunit *test)
{
	static struct rlimit saved;
	struct kstat st;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G394_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g394_remove_tree, NULL),
			0);

	saved = current->signal->rlim[RLIMIT_FSIZE];
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g394_restore_rlimit,
						  &saved), 0);
	current->signal->rlim[RLIMIT_FSIZE] =
		(struct rlimit){ G394_LIMIT, saved.rlim_max };

	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G394_BELOW, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G394_AT, "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G394_ABOVE, "", 0), 0);

	KUNIT_EXPECT_EQ_MSG(test, xfs_truncate(G394_BELOW, G394_LIMIT - 1), 0,
			    "truncating to one byte below the limit failed");
	KUNIT_EXPECT_EQ_MSG(test, xfs_truncate(G394_AT, G394_LIMIT), 0,
			    "truncating to exactly the limit failed");
	KUNIT_EXPECT_EQ_MSG(test, xfs_truncate(G394_ABOVE, G394_LIMIT + 1),
			    -EFBIG,
			    "truncating past the limit was allowed");

	KUNIT_ASSERT_EQ(test, xfs_kstat(G394_AT, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G394_LIMIT,
			    "the server holds %lld bytes", st.size);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G394_ABOVE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, 0LL,
			    "the refused truncate left %lld bytes behind",
			    st.size);
}

static int g394_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g394_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g394_cases[] = {
	KUNIT_CASE(truncate_stops_at_the_file_size_limit),
	{}
};

static struct kunit_suite g394_suite = {
	.name		= "xfstests/generic/394",
	.suite_init	= g394_suite_init,
	.suite_exit	= g394_suite_exit,
	.test_cases	= g394_cases,
};

kunit_test_suites(&g394_suite);

MODULE_DESCRIPTION("xfstests generic/394 over a loopback NFS mount");
MODULE_LICENSE("GPL");
