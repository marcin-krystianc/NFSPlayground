// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/193 over a loopback NFS mount: permission checks in
 * ->setattr.
 *
 * Upstream creates two files, test.root owned by root and test.user
 * owned by qa_user, and runs each check as root or as qa_user:
 *
 *	ATTR_UID: which chowns qa_user may make
 *	ATTR_GID: which chgrps qa_user may make
 *	ATTR_MODE: which chmods qa_user may make; qa_user's chmod clears
 *	  sgid on a file whose group it is not in; root's does not clear suid
 *	suid/sgid after root's chown, in four exec-bit combinations
 *	suid/sgid after qa_user truncates and writes ("echo > file"), in the
 *	  same four combinations
 *	ATTR_*TIMES_SET: qa_user may touch its own file but not root's
 *
 * The expected modes are upstream's golden output. Over NFS the clearing
 * is the server's: the client drops ATTR_MODE when it sees ATTR_KILL_*
 * (nfs_setattr()), and knfsd applies the rule under the caller's
 * credentials.
 *
 * Each section is a test case, with the files created as upstream's
 * _create_files does. qa_user is uid/gid 99 with no capabilities
 * (xfs_switch_creds()). chmod and chown are issued the way coreutils
 * issues them: chmod computes the new mode from the current one, and
 * chown/chgrp pass -1 for the id they leave alone. touch is utimensat()
 * with both times UTIME_NOW.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G193_ROOT	XFS_MNT "/g193"
#define G193_TROOT	G193_ROOT "/test.root"
#define G193_TUSER	G193_ROOT "/test.user"
#define G193_QA		99
#define G193_KEEP	((uid_t)-1)

static void g193_creds_action(void *unused)
{
	xfs_restore_creds();
}

static void g193_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G193_TUSER);
	xfs_unlink(G193_TROOT);
	xfs_rmdir_settled(G193_ROOT);
}

/* touch: create if missing, never truncate */
static void g193_touch_create(struct kunit *test, const char *path)
{
	struct file *f = filp_open(path, O_WRONLY | O_CREAT, 0644);

	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "creating %s: %ld", path,
			       PTR_ERR(f));
	filp_close(f, NULL);
}

/* _create_files */
static void g193_create_files(struct kunit *test)
{
	g193_touch_create(test, G193_TROOT);
	g193_touch_create(test, G193_TUSER);
	KUNIT_ASSERT_EQ(test, xfs_chown(G193_TUSER, G193_QA, G193_QA), 0);
}

static void g193_setup(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G193_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g193_remove_tree, NULL),
			0);
	g193_create_files(test);
}

static void g193_as_qa_user(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(G193_QA, G193_QA), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g193_creds_action, NULL),
			0);
}

static void g193_as_root(struct kunit *test)
{
	kunit_release_action(test, g193_creds_action, NULL);
}

static umode_t g193_mode(struct kunit *test, const char *path)
{
	struct kstat st;

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	return st.mode & 07777;
}

/* chmod(1) with a symbolic mode: the new mode is computed from the old */
static int g193_chmod(struct kunit *test, const char *path, umode_t set,
		      umode_t clear)
{
	return xfs_chmod(path, (g193_mode(test, path) | set) & ~clear);
}

#define g193_expect_mode(test, path, want, what)			\
	do {								\
		umode_t __m = g193_mode(test, path);			\
		KUNIT_EXPECT_EQ_MSG(test, __m, (umode_t)(want),		\
				    "%s: mode %04o, expected %04o",	\
				    what, __m, (umode_t)(want));	\
	} while (0)

#define g193_expect_err(test, call, want, what)				\
	do {								\
		int __e = (call);					\
		KUNIT_EXPECT_EQ_MSG(test, __e, want, "%s: got %d",	\
				    what, __e);				\
	} while (0)

static void attr_uid(struct kunit *test)
{
	g193_setup(test);
	g193_as_qa_user(test);

	g193_expect_err(test, xfs_chown(G193_TROOT, G193_QA, G193_KEEP),
			-EPERM, "chown root owned file to qa_user");
	g193_expect_err(test, xfs_chown(G193_TROOT, 0, G193_KEEP), -EPERM,
			"chown root owned file to root");
	g193_expect_err(test, xfs_chown(G193_TUSER, G193_QA, G193_KEEP), 0,
			"chown qa_user owned file to qa_user");
	/* this would work without _POSIX_CHOWN_RESTRICTED */
	g193_expect_err(test, xfs_chown(G193_TUSER, 0, G193_KEEP), -EPERM,
			"chown qa_user owned file to root");
}

static void attr_gid(struct kunit *test)
{
	g193_setup(test);
	g193_as_qa_user(test);

	g193_expect_err(test, xfs_chown(G193_TROOT, G193_KEEP, 0), -EPERM,
			"chgrp root owned file to root");
	g193_expect_err(test, xfs_chown(G193_TUSER, G193_KEEP, 0), -EPERM,
			"chgrp qa_user owned file to root");
	g193_expect_err(test, xfs_chown(G193_TROOT, G193_KEEP, G193_QA),
			-EPERM, "chgrp root owned file to qa_user");
	g193_expect_err(test, xfs_chown(G193_TUSER, G193_KEEP, G193_QA), 0,
			"chgrp qa_user owned file to qa_user");
}

static void attr_mode(struct kunit *test)
{
	g193_setup(test);

	g193_as_qa_user(test);
	g193_expect_err(test, g193_chmod(test, G193_TUSER, 0444, 0), 0,
			"chmod a+r on qa_user owned file");
	g193_expect_err(test, g193_chmod(test, G193_TROOT, 0444, 0), -EPERM,
			"chmod a+r on root owned file");
	g193_as_root(test);

	/* qa_user's file, group root, sgid: qa_user's chmod must clear it */
	KUNIT_ASSERT_EQ(test, xfs_chown(G193_TUSER, G193_QA, 0), 0);
	KUNIT_ASSERT_EQ(test, g193_chmod(test, G193_TUSER, S_ISGID, 0), 0);
	g193_as_qa_user(test);
	g193_expect_err(test, g193_chmod(test, G193_TUSER, 0222, 0), 0,
			"chmod a+w on qa_user owned sgid file");
	g193_as_root(test);
	g193_expect_mode(test, G193_TUSER, 0666, "the sgid bit is cleared");

	/* root's chmod leaves suid alone */
	KUNIT_ASSERT_EQ(test, g193_chmod(test, G193_TUSER, S_ISUID, 0), 0);
	KUNIT_ASSERT_EQ(test, g193_chmod(test, G193_TUSER, 0222, 0), 0);
	g193_expect_mode(test, G193_TUSER, 04666, "suid bit is not cleared");
}

/* the four exec-bit combinations, and the modes upstream expects */
static const struct {
	const char *what;
	umode_t set, clear;	/* applied after chmod ug+s */
	umode_t before, after;
} g193_combos[] = {
	{ "with no exec perm",		0,	0,	06644, 02644 },
	{ "with user exec perm",	0100,	0,	06744, 02744 },
	{ "with group exec perm",	0010,	0100,	06654, 00654 },
	{ "with user+group exec perm",	0110,	0,	06754, 00754 },
};

static void g193_ug_s(struct kunit *test, int i)
{
	KUNIT_ASSERT_EQ(test,
			g193_chmod(test, G193_TUSER, S_ISUID | S_ISGID, 0), 0);
	if (g193_combos[i].set)
		KUNIT_ASSERT_EQ(test,
				g193_chmod(test, G193_TUSER,
					   g193_combos[i].set, 0), 0);
	if (g193_combos[i].clear)
		KUNIT_ASSERT_EQ(test,
				g193_chmod(test, G193_TUSER, 0,
					   g193_combos[i].clear), 0);
	g193_expect_mode(test, G193_TUSER, g193_combos[i].before,
			 g193_combos[i].what);
}

static void chown_clears_suid_and_sgid(struct kunit *test)
{
	int i;

	g193_setup(test);

	for (i = 0; i < ARRAY_SIZE(g193_combos); i++) {
		g193_ug_s(test, i);
		KUNIT_EXPECT_EQ(test, xfs_chown(G193_TUSER, 0, G193_KEEP), 0);
		g193_expect_mode(test, G193_TUSER, g193_combos[i].after,
				 g193_combos[i].what);
	}
}

/* echo frobnozzle >> file */
static void g193_append(struct kunit *test, const char *path)
{
	static const char msg[] = "frobnozzle\n";
	struct file *f = filp_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	loff_t pos = 0;

	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	KUNIT_EXPECT_EQ(test, kernel_write(f, msg, sizeof(msg) - 1, &pos),
			(ssize_t)(sizeof(msg) - 1));
	filp_close(f, NULL);
}

static void truncate_clears_suid_and_sgid(struct kunit *test)
{
	int i;

	g193_setup(test);

	for (i = 0; i < ARRAY_SIZE(g193_combos); i++) {
		g193_append(test, G193_TUSER);
		g193_ug_s(test, i);
		/* echo > file, as qa_user: O_TRUNC, then a one-byte write */
		g193_as_qa_user(test);
		KUNIT_EXPECT_EQ(test, xfs_write_new_file(G193_TUSER, "\n", 1),
				0);
		g193_as_root(test);
		g193_expect_mode(test, G193_TUSER, g193_combos[i].after,
				 g193_combos[i].what);
	}
}

static void attr_times_set(struct kunit *test)
{
	struct timespec64 now[2] = {
		{ .tv_nsec = UTIME_NOW }, { .tv_nsec = UTIME_NOW },
	};

	g193_setup(test);
	g193_as_qa_user(test);

	g193_expect_err(test, xfs_utimes_raw(G193_TUSER, now), 0,
			"touch qa_user file");
	g193_expect_err(test, xfs_utimes_raw(G193_TROOT, now), -EACCES,
			"touch root file");
}

static int g193_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g193_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g193_cases[] = {
	KUNIT_CASE(attr_uid),
	KUNIT_CASE(attr_gid),
	KUNIT_CASE(attr_mode),
	KUNIT_CASE(chown_clears_suid_and_sgid),
	KUNIT_CASE(truncate_clears_suid_and_sgid),
	KUNIT_CASE(attr_times_set),
	{}
};

static struct kunit_suite g193_suite = {
	.name		= "xfstests/generic/193",
	.suite_init	= g193_suite_init,
	.suite_exit	= g193_suite_exit,
	.test_cases	= g193_cases,
};

kunit_test_suites(&g193_suite);

MODULE_DESCRIPTION("xfstests generic/193 over a loopback NFS mount");
MODULE_LICENSE("GPL");
