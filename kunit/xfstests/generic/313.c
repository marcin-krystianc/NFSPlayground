// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/313 over a loopback NFS mount: ctime and mtime on truncate.
 *
 * src/t_truncate_cmtime runs one sequence twice -- once through truncate(2)
 * and once through ftruncate(2): create the file, write a short string, stat,
 * truncate down to 0, stat and require both ctime and mtime to have moved,
 * then truncate up to 123 and require them to have moved again.
 *
 * Both halves matter over NFS and they are not the same RPC: truncate(2) is
 * a SETATTR with no stateid, ftruncate(2) carries the open file's stateid
 * (and can therefore be served while another client holds a delegation).
 * The times come from the server either way, so they are read back with
 * FORCE_SYNC rather than from the client's cached inode.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G313_ROOT	XFS_MNT "/g313"
#define G313_FILE	G313_ROOT "/f"

/* upstream's TEST_MSG, written with its trailing NUL as sizeof() does */
static const char g313_msg[] = "this is a test string";

/* strictly-after comparison for timestamps */
static bool g313_after(const struct timespec64 *a, const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void g313_remove_tree(void *unused)
{
	xfs_unlink(G313_FILE);
	xfs_rmdir(G313_ROOT);
}

/* one truncate step, checked the way t_truncate_cmtime checks it */
static void g313_step(struct kunit *test, struct file *f, bool use_ftruncate,
		      loff_t newsize, struct kstat *prev, const char *what)
{
	struct kstat st;
	int err;

	msleep(20);	/* upstream's sleep(1), scaled */

	if (use_ftruncate)
		err = xfs_ftruncate(f, newsize);
	else
		err = xfs_truncate(G313_FILE, newsize);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s %s failed: %d",
			    use_ftruncate ? "ftruncate" : "truncate", what, err);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G313_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, newsize,
			    "%s %s left the size at %lld",
			    use_ftruncate ? "ftruncate" : "truncate", what,
			    st.size);
	KUNIT_EXPECT_TRUE_MSG(test, g313_after(&st.ctime, &prev->ctime),
			      "ctime not updated after %s %s",
			      use_ftruncate ? "ftruncate" : "truncate", what);
	KUNIT_EXPECT_TRUE_MSG(test, g313_after(&st.mtime, &prev->mtime),
			      "mtime not updated after %s %s",
			      use_ftruncate ? "ftruncate" : "truncate", what);
	*prev = st;
}

/* do_test(): the whole sequence for one of the two truncate flavours */
static void g313_do_test(struct kunit *test, bool use_ftruncate)
{
	struct kstat st;
	struct file *f;
	loff_t pos = 0;

	f = filp_open(G313_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test,
			kernel_write(f, g313_msg, sizeof(g313_msg), &pos),
			(ssize_t)sizeof(g313_msg));

	KUNIT_ASSERT_EQ(test, xfs_kstat(G313_FILE, &st), 0);

	g313_step(test, f, use_ftruncate, 0, &st, "down");
	g313_step(test, f, use_ftruncate, 123, &st, "up");

	filp_close(f, NULL);
}

static void truncate_updates_ctime_and_mtime(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G313_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g313_remove_tree, NULL),
			0);

	g313_do_test(test, false);
}

static void ftruncate_updates_ctime_and_mtime(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G313_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g313_remove_tree, NULL),
			0);

	g313_do_test(test, true);
}

static int g313_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g313_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g313_cases[] = {
	KUNIT_CASE(truncate_updates_ctime_and_mtime),
	KUNIT_CASE(ftruncate_updates_ctime_and_mtime),
	{}
};

static struct kunit_suite g313_suite = {
	.name		= "xfstests/generic/313",
	.suite_init	= g313_suite_init,
	.suite_exit	= g313_suite_exit,
	.test_cases	= g313_cases,
};

kunit_test_suites(&g313_suite);

MODULE_DESCRIPTION("xfstests generic/313 over a loopback NFS mount");
MODULE_LICENSE("GPL");
