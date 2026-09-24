// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/618 over a loopback NFS mount: two mid-sized xattrs on
 * one file.
 *
 * Upstream sets user.0 and user.1 to the same 232-byte value (the output
 * of "seq 0 80", less the trailing newline that the backquotes strip),
 * cycles the mount and dumps both back. On XFS the second set is what
 * used to underflow the fork-offset calculation; the test is kept generic
 * because the sequence is ordinary.
 *
 * Over NFSv4.2 it is two SETXATTRs of a value a few hundred bytes long,
 * which is past the point where the value stops fitting alongside the
 * name in one small buffer on either side. The port checks both values
 * byte for byte through the client and again through the server's own
 * copy -- upstream's mount cycle exists to defeat the same caching the
 * export check defeats here.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G618_ROOT	XFS_MNT "/g618"
#define G618_FILE	G618_ROOT "/testfile"
#define G618_SERVER	XFS_EXPORT "/g618/testfile"
#define G618_VALSZ	256

static void g618_remove_tree(void *unused)
{
	xfs_unlink(G618_FILE);
	xfs_rmdir_settled(G618_ROOT);
}

/* upstream's `seq 0 80`: the numbers 0..80, one per line, no final newline */
static size_t g618_build_value(char *buf, size_t size)
{
	size_t len = 0;
	int i;

	for (i = 0; i <= 80 && len < size; i++)
		len += scnprintf(buf + len, size - len, "%d\n", i);
	return len - 1;
}

static void g618_check(struct kunit *test, const char *path, const char *name,
		       const char *want, size_t len, const char *which)
{
	char got[G618_VALSZ];
	ssize_t n = xfs_getxattr(path, name, got, sizeof(got));

	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
			    "%s: %s returned %zd bytes, expected %zu", which,
			    name, n, len);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(got, want, len), 0,
			    "%s: %s holds the wrong value", which, name);
}

static void two_mid_sized_xattrs_both_survive(struct kunit *test)
{
	char value[G618_VALSZ];
	size_t len;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G618_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g618_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G618_FILE, "", 0), 0);

	len = g618_build_value(value, sizeof(value));
	KUNIT_ASSERT_EQ(test, len, 232UL);

	KUNIT_ASSERT_EQ(test,
			xfs_setxattr(G618_FILE, "user.0", value, len, 0), 0);
	KUNIT_ASSERT_EQ_MSG(test,
			    xfs_setxattr(G618_FILE, "user.1", value, len, 0), 0,
			    "the second attribute could not be set");

	g618_check(test, G618_FILE, "user.0", value, len, "client");
	g618_check(test, G618_FILE, "user.1", value, len, "client");
	g618_check(test, G618_SERVER, "user.0", value, len, "server");
	g618_check(test, G618_SERVER, "user.1", value, len, "server");
}

static int g618_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g618_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g618_cases[] = {
	KUNIT_CASE(two_mid_sized_xattrs_both_survive),
	{}
};

static struct kunit_suite g618_suite = {
	.name		= "xfstests/generic/618",
	.suite_init	= g618_suite_init,
	.suite_exit	= g618_suite_exit,
	.test_cases	= g618_cases,
};

kunit_test_suites(&g618_suite);

MODULE_DESCRIPTION("xfstests generic/618 over a loopback NFS mount");
MODULE_LICENSE("GPL");
