// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/087 over a loopback NFS mount: who may set a file's times.
 *
 * Upstream drives src/fs_perms with the "t" and "T" opcodes, which are not
 * truncate: fs_perms.c does utime("test.file", NULL) for "t" and
 * utime("test.file", &times) for "T". The rule from utime(2), quoted in
 * upstream's own header, is that changing timestamps is permitted when the
 * caller has privileges, or owns the file, or -- for the current-time form
 * only -- has write permission. So the two forms diverge for a non-owner
 * who can write: "t" is allowed and "T" is not.
 *
 * In the kernel the same split is ATTR_TOUCH (times == NULL, checked against
 * MAY_WRITE) versus ATTR_TIMES_SET (explicit times, owner-only); over NFSv4
 * it is SET_TO_SERVER_TIME4 versus SET_TO_CLIENT_TIME4 in the SETATTR, and
 * the server does the checking. That is why upstream notes this test
 * "will always wrongly succeed over NFSv2" and did so for NFSv3+ until the
 * commit "Disable NFSv2 timestamp workaround for NFSv3+" -- the row that
 * catches it is the last one below.
 *
 * The six rows are upstream's six fs_perms invocations, in order. As with
 * fs_perms the verdict compared is allowed-or-denied, not a specific errno.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G087_ROOT	XFS_MNT "/g087"

static void g087_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G087_FILE	G087_ROOT "/testx.file"

/* $QA_FS_PERMS <file mode> <file uid> <file gid> <uid> <gid> <t|T> <expect> */
static const struct g087_row {
	umode_t	mode;
	uid_t	fuid;	gid_t fgid;
	uid_t	uid;	gid_t gid;
	bool	explicit_times;		/* false = "t", true = "T" */
	bool	allowed;
} g087_rows[] = {
	{ 0600, 99, 99,  99, 99, false, true  },	/* 600 99 99 99 99 t 1 */
	{ 0600, 99, 99,  99, 99, true,  true  },	/* 600 99 99 99 99 T 1 */
	{ 0600, 99, 99, 100, 99, false, false },	/* 600 99 99 100 99 t 0 */
	{ 0600, 99, 99, 100, 99, true,  false },	/* 600 99 99 100 99 T 0 */
	{ 0660, 99, 99, 100, 99, false, true  },	/* 660 99 99 100 99 t 1 */
	{ 0660, 99, 99, 100, 99, true,  false },	/* 660 99 99 100 99 T 0 */
};

static void g087_remove_tree(void *unused)
{
	xfs_unlink(G087_FILE);
	xfs_rmdir(G087_ROOT);
}

/* fs_perms' two forms: utime(f, NULL) and utime(f, &times) */
static int g087_set_times(bool explicit_times)
{
	struct timespec64 times[2] = {
		{ .tv_sec = 1000000000, .tv_nsec = 0 },
		{ .tv_sec = 1000000000, .tv_nsec = 0 },
	};

	if (explicit_times)
		return xfs_utimes_raw(G087_FILE, times);
	return xfs_utimes_raw(G087_FILE, NULL);
}

static void setting_times_follows_utime_permissions(struct kunit *test)
{
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G087_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G087_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_creds_action, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(g087_rows); i++) {
		const struct g087_row *row = &g087_rows[i];
		int err;

		/* root sets the stage, as fs_perms' testsetup() does */
		xfs_unlink(G087_FILE);
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(G087_FILE, "tttt", 4), 0);
		KUNIT_ASSERT_EQ(test,
				xfs_chown(G087_FILE, row->fuid, row->fgid), 0);
		KUNIT_ASSERT_EQ(test, xfs_chmod(G087_FILE, row->mode), 0);

		KUNIT_ASSERT_EQ(test,
				xfs_switch_creds(row->uid, row->gid), 0);
		err = g087_set_times(row->explicit_times);
		xfs_restore_creds();

		KUNIT_EXPECT_EQ_MSG(test, err == 0, row->allowed,
				    "row %d: %s a %03o file owned by (%d/%d) as (%d/%d) -- got %d",
				    i, row->explicit_times ? "T" : "t",
				    row->mode, row->fuid, row->fgid, row->uid,
				    row->gid, err);
	}
}

/*
 * The same split stated directly, so a failure above can be read: the
 * current-time form needs write permission, the explicit form needs
 * ownership. This is the pair upstream's last two rows disagree on.
 */
static void current_time_needs_write_explicit_needs_ownership(struct kunit *test)
{
	struct kstat before, after;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G087_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G087_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g087_creds_action, NULL),
			0);

	xfs_unlink(G087_FILE);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G087_FILE, "tttt", 4), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G087_FILE, 99, 99), 0);
	/* group-writable, and the actor is in that group but does not own it */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G087_FILE, 0660), 0);
	KUNIT_ASSERT_EQ(test, xfs_utimes(G087_FILE, 1000, 1000), 0);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G087_FILE, &before), 0);

	KUNIT_ASSERT_EQ(test, xfs_switch_creds(100, 99), 0);
	err = xfs_utimes_raw(G087_FILE, NULL);
	KUNIT_EXPECT_EQ_MSG(test, err, 0,
			    "a writer could not set the times to the current time: %d",
			    err);

	err = xfs_utimes(G087_FILE, 555, 555);
	xfs_restore_creds();
	KUNIT_EXPECT_NE_MSG(test, err, 0,
			    "a non-owner set explicit timestamps -- the NFSv2 timestamp workaround is back");

	/* and the refused SETATTR left the explicit times alone */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G087_FILE, &after), 0);
	KUNIT_EXPECT_NE_MSG(test, after.mtime.tv_sec, (time64_t)555,
			    "the denied explicit SETATTR landed anyway");
	KUNIT_EXPECT_TRUE_MSG(test, after.mtime.tv_sec != before.mtime.tv_sec,
			      "the allowed current-time SETATTR did not move mtime");
}

static int g087_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g087_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g087_cases[] = {
	KUNIT_CASE(setting_times_follows_utime_permissions),
	KUNIT_CASE(current_time_needs_write_explicit_needs_ownership),
	{}
};

static struct kunit_suite g087_suite = {
	.name		= "xfstests/generic/087",
	.suite_init	= g087_suite_init,
	.suite_exit	= g087_suite_exit,
	.test_cases	= g087_cases,
};

kunit_test_suites(&g087_suite);

MODULE_DESCRIPTION("xfstests generic/087 over a loopback NFS mount");
MODULE_LICENSE("GPL");
