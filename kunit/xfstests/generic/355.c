// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/355 over a loopback NFS mount: suid and sgid after a
 * direct write by the file's owner.
 *
 * Upstream creates a file owned by an unprivileged user, sets its mode
 * four different ways, and after each one has that user do "xfs_io -d -c
 * 'pwrite 0 4k'" and prints the mode. Its golden output encodes the rule:
 *
 *	-rwSr-Sr--  ->  -rw-r-Sr--	no exec bits
 *	-rwsr-Sr--  ->  -rwxr-Sr--	user exec
 *	-rwSr-sr--  ->  -rw-r-xr--	group exec
 *	-rwsr-sr--  ->  -rwxr-xr--	user and group exec
 *
 * That is, a write by a non-root writer always clears S_ISUID, and clears
 * S_ISGID only when the file is group-executable -- S_ISGID without
 * group-exec means mandatory locking, not "run as group", so it is left
 * alone (setattr_should_drop_suidgid()).
 *
 * Over NFS the stripping is the server's job: the client's WRITE carries
 * no mode, knfsd applies the rule on the underlying file, and the client
 * only finds out by asking again. The port therefore checks both the
 * server's own mode through the tmpfs export and what the client reports
 * after a forced revalidation, so a client that keeps serving a stale
 * setuid mode from its cache is visible rather than hidden behind the
 * server's correct answer.
 *
 * The write goes through xfs_direct_write() (O_DIRECT from a kernel
 * buffer, which kernel_write() cannot do over NFS) as the file's owner,
 * with capabilities dropped so the rule actually applies.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G355_ROOT	XFS_MNT "/g355"
#define G355_FILE	G355_ROOT "/355.test"
#define G355_SERVER	XFS_EXPORT "/g355/355.test"

#define G355_UID	1000
#define G355_GID	1000
#define G355_LEN	4096

static const struct g355_case {
	const char	*name;
	umode_t		before;
	umode_t		after;
} g355_cases_table[] = {
	{ "no exec perm",		06644,	02644 },
	{ "user exec perm",		06744,	02744 },
	{ "group exec perm",		06654,	00654 },
	{ "user+group exec perm",	06754,	00754 },
};

static void g355_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G355_FILE);
	xfs_rmdir_settled(G355_ROOT);
}

static void a_direct_write_clears_suid_by_the_posix_rule(struct kunit *test)
{
	struct kstat st;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G355_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g355_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G355_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 'w', G355_LEN);

	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G355_FILE, "this is a test\n", 15),
			0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G355_FILE, 0644), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G355_FILE, G355_UID, G355_GID), 0);

	for (i = 0; i < ARRAY_SIZE(g355_cases_table); i++) {
		const struct g355_case *c = &g355_cases_table[i];
		struct file *f;
		loff_t pos = 0;
		ssize_t n;

		KUNIT_ASSERT_EQ_MSG(test, xfs_chmod(G355_FILE, c->before), 0,
				    "%s: chmod %o failed", c->name, c->before);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G355_FILE, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.mode & 07777, c->before,
				    "%s: mode before the write is %o",
				    c->name, st.mode & 07777);

		KUNIT_ASSERT_EQ(test, xfs_switch_creds(G355_UID, G355_GID), 0);
		f = filp_open(G355_FILE, O_RDWR | O_DIRECT, 0);
		if (IS_ERR(f)) {
			xfs_restore_creds();
			KUNIT_FAIL(test, "%s: open as the owner: %ld", c->name,
				   PTR_ERR(f));
			return;
		}
		n = xfs_direct_write(f, buf, G355_LEN, &pos);
		filp_close(f, NULL);
		xfs_restore_creds();
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G355_LEN,
				    "%s: direct write returned %zd", c->name,
				    n);

		KUNIT_ASSERT_EQ(test, xfs_kstat(G355_SERVER, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 07777, c->after,
				    "%s: the server left the mode at %o, expected %o",
				    c->name, st.mode & 07777, c->after);

		KUNIT_ASSERT_EQ(test, xfs_kstat(G355_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 07777, c->after,
				    "%s: the client reports mode %o, expected %o",
				    c->name, st.mode & 07777, c->after);
	}
}

static int g355_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g355_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g355_cases[] = {
	KUNIT_CASE(a_direct_write_clears_suid_by_the_posix_rule),
	{}
};

static struct kunit_suite g355_suite = {
	.name		= "xfstests/generic/355",
	.suite_init	= g355_suite_init,
	.suite_exit	= g355_suite_exit,
	.test_cases	= g355_cases,
};

kunit_test_suites(&g355_suite);

MODULE_DESCRIPTION("xfstests generic/355 over a loopback NFS mount");
MODULE_LICENSE("GPL");
