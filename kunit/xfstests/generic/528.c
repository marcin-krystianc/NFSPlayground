// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/528 over a loopback NFS mount: statx btime is when the
 * file was created.
 *
 * Upstream records the wall clock, touches a file, asks statx for
 * STATX_BTIME and requires the answer to be within five seconds of the
 * recorded time. It was written because nothing checked btime values at
 * all.
 *
 * Over NFSv4 btime is the time_create attribute, which the client asks
 * for only when the caller asked for STATX_BTIME (nfs_getattr() adds it
 * to the bitmap) and which the server fills from the underlying
 * filesystem -- tmpfs keeps it. So the port checks three things
 * upstream's tolerance check implies: that the value is close to now,
 * that it does not move when the file is written to afterwards (which is
 * what makes it a creation time rather than another mtime), and that the
 * bit comes back in stx_mask at all.
 *
 * That last one is a requirement upstream states separately, as
 * _require_btime, and it is the one that needs care here: **the NFS
 * client only learned to report btime after v6.12.57**. fs/nfs/inode.c
 * has no STATX_BTIME at all on that release and five references to it on
 * v6.18.52, v7.2.6 and mainline, so on the oldest kernel in this repo's
 * CI matrix the mount answers with the bit clear, and upstream notruns.
 *
 * Skipping unconditionally would give that up on every other kernel too:
 * a client that stopped reporting btime would skip quietly instead of
 * failing. So the two cases are told apart at compile time by
 * NFS_ATTR_FATTR_BTIME (include/linux/nfs_xdr.h), which is absent on
 * v6.12.57 and present on v6.18.52, v7.2.6 and mainline -- the same
 * boundary as the behaviour. Where the client knows about btime, not
 * reporting it is a failure; where it does not, the case skips.
 *
 * Deviations: the tolerance is the same five seconds; the clock is the
 * kernel's own coarse real time rather than date(1).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/time64.h>
#include <linux/delay.h>
#include <linux/ktime.h>
/* for NFS_ATTR_FATTR_BTIME; nfs_xdr.h does not stand alone, nfs_fs.h
 * pulls it in with the nfs.h/nfs4.h/sunrpc chain it needs
 */
#include <linux/nfs_fs.h>

#include "xfstests_nfs_fixture.h"

#define G528_ROOT	XFS_MNT "/g528"
#define G528_FILE	G528_ROOT "/528.txt"
#define G528_SLACK	5	/* upstream's tolerance, in seconds */

static void g528_remove_tree(void *unused)
{
	xfs_unlink(G528_FILE);
	xfs_rmdir_settled(G528_ROOT);
}

/* vfs_getattr asking for btime specifically */
static int g528_btime(const char *path, struct kstat *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_getattr(&p, st, STATX_BASIC_STATS | STATX_BTIME,
			  AT_STATX_FORCE_SYNC);
	path_put(&p);
	return err;
}

static void btime_is_when_the_file_was_created(struct kunit *test)
{
	struct timespec64 now;
	struct kstat st, after;
	s64 delta;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G528_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g528_remove_tree, NULL),
			0);

	ktime_get_real_ts64(&now);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G528_FILE, "", 0), 0);

	KUNIT_ASSERT_EQ(test, g528_btime(G528_FILE, &st), 0);
#ifdef NFS_ATTR_FATTR_BTIME
	KUNIT_ASSERT_TRUE_MSG(test, st.result_mask & STATX_BTIME,
			      "this client knows about btime but did not report it (mask %x)",
			      st.result_mask);
#else
	if (!(st.result_mask & STATX_BTIME))
		kunit_skip(test,
			   "this client has no btime support at all (mask %x); fs/nfs gained it after v6.12.57",
			   st.result_mask);
#endif

	delta = st.btime.tv_sec - now.tv_sec;
	KUNIT_EXPECT_TRUE_MSG(test, delta >= -G528_SLACK && delta <= G528_SLACK,
			      "btime is %lld, %lld seconds away from now (%lld)",
			      st.btime.tv_sec, delta, now.tv_sec);

	/* writing to the file moves mtime, and must not move btime */
	msleep(20);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G528_FILE, "hello", 5), 0);
	KUNIT_ASSERT_EQ(test, g528_btime(G528_FILE, &after), 0);
	KUNIT_EXPECT_EQ_MSG(test, after.btime.tv_sec, st.btime.tv_sec,
			    "btime moved from %lld to %lld after a write",
			    st.btime.tv_sec, after.btime.tv_sec);
	KUNIT_EXPECT_EQ_MSG(test, after.btime.tv_nsec, st.btime.tv_nsec,
			    "btime's nanoseconds moved from %ld to %ld after a write",
			    st.btime.tv_nsec, after.btime.tv_nsec);
}

static int g528_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g528_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g528_cases[] = {
	KUNIT_CASE(btime_is_when_the_file_was_created),
	{}
};

static struct kunit_suite g528_suite = {
	.name		= "xfstests/generic/528",
	.suite_init	= g528_suite_init,
	.suite_exit	= g528_suite_exit,
	.test_cases	= g528_cases,
};

kunit_test_suites(&g528_suite);

MODULE_DESCRIPTION("xfstests generic/528 over a loopback NFS mount");
MODULE_LICENSE("GPL");
