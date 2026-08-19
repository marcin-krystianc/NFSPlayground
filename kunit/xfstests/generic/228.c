// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/228 over a loopback NFS mount: RLIMIT_FSIZE and fallocate.
 *
 * Upstream: fallocate must respect RLIMIT_FSIZE. KUnit cases run in
 * kthreads with their own signal struct, so the limit can be set and
 * restored locally: with FSIZE at 64K, allocating or writing past it
 * must fail EFBIG, at it must succeed, and the pre-set limit state is
 * restored afterwards. (The EFBIG paths also raise SIGXFSZ, which a
 * kthread ignores.)
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/resource.h>
#include <linux/sched/signal.h>

#include "xfstests_nfs_fixture.h"

#define G228_ROOT	XFS_MNT "/g228"

#define G228_FILE	G228_ROOT "/f"
#define G228_LIMIT	(64 * 1024)

static void g228_remove_tree(void *unused)
{
	xfs_unlink(G228_FILE);
	xfs_rmdir(G228_ROOT);
}

static void g228_restore_rlimit(void *arg)
{
	struct rlimit *saved = arg;

	current->signal->rlim[RLIMIT_FSIZE] = *saved;
}

static void fallocate_respects_rlimit_fsize(struct kunit *test)
{
	static struct rlimit saved;
	struct file *f;
	loff_t pos;
	u8 b = 1;
	ssize_t n;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G228_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g228_remove_tree, NULL),
			0);

	saved = current->signal->rlim[RLIMIT_FSIZE];
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g228_restore_rlimit,
						  &saved), 0);
	current->signal->rlim[RLIMIT_FSIZE] =
		(struct rlimit){ G228_LIMIT, G228_LIMIT };

	f = filp_open(G228_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	/* up to the limit is fine */
	err = vfs_fallocate(f, 0, 0, G228_LIMIT);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "allocation up to RLIMIT_FSIZE refused: %d", err);

	/* past it: EFBIG, for fallocate and write alike */
	err = vfs_fallocate(f, 0, 0, G228_LIMIT + 4096);
	KUNIT_EXPECT_EQ_MSG(test, err, -EFBIG,
			    "allocation past RLIMIT_FSIZE: expected EFBIG, got %d",
			    err);
	pos = G228_LIMIT;
	n = kernel_write(f, &b, 1, &pos);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)-EFBIG,
			    "write past RLIMIT_FSIZE: expected EFBIG, got %zd",
			    n);
	filp_close(f, NULL);

	/* the size never exceeded the limit */
	{
		struct kstat st;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G228_FILE, &st), 0);
		KUNIT_EXPECT_EQ(test, st.size, (loff_t)G228_LIMIT);
	}
}

static int g228_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g228_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g228_cases[] = {
	KUNIT_CASE(fallocate_respects_rlimit_fsize),
	{}
};

static struct kunit_suite g228_suite = {
	.name		= "xfstests/generic/228",
	.suite_init	= g228_suite_init,
	.suite_exit	= g228_suite_exit,
	.test_cases	= g228_cases,
};

kunit_test_suites(&g228_suite);

MODULE_DESCRIPTION("xfstests generic/228 over a loopback NFS mount");
MODULE_LICENSE("GPL");
