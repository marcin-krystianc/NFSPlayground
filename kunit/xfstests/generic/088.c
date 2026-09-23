// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/088 over a loopback NFS mount: CAP_DAC_OVERRIDE in
 * access(2)'s real-uid override path.
 *
 * Upstream's src/t_access_root: chown a mode-000 file to uid 500, then
 * seteuid(500) (NOT setuid -- the real uid stays 0/root) and probe every
 * access(2) mask. The golden 088.out shows R_OK/W_OK succeeding and X_OK
 * failing despite the mode-000 file granting nobody anything and despite
 * the caller's *effective* uid being a non-owner-by-permission-bits 500.
 *
 * The reason is POSIX, not a bug: access(2) is specified to check the
 * *real* uid/gid, not the effective ones, precisely so a setuid-root
 * binary can ask "could the invoking user do this" without being fooled
 * by its own elevated privilege. fs/open.c's access_override_creds()
 * implements this: it builds a cred with fsuid pinned to the *real* uid,
 * and -- this is the code the test's own comment names, "CAP_DAC_OVERRIDE
 * and CAP_DAC_SEARCH code in xfs_iaccess" -- restores cap_effective to
 * cap_permitted when that real uid is 0. Since the test never changes the
 * real uid, access() evaluates as full root regardless of the seteuid()
 * call: R_OK/W_OK bypass permission bits via CAP_DAC_OVERRIDE, and X_OK
 * still fails because generic_permission() only lets DAC_OVERRIDE satisfy
 * MAY_EXEC when at least one x bit is set somewhere in the mode -- mode
 * 000 has none. xfs_access() (nfs_fixture.c) reproduces this override
 * construction; xfs_seteuid() reproduces seteuid(2)'s actual effect (only
 * euid/fsuid move) via the same security_task_fix_setuid() LSM hook
 * setresuid(2) goes through, rather than hand-rolling the capability math.
 *
 * An earlier version of this port used filp_open() under a cred built by
 * xfs_switch_creds(), which moves uid/euid/suid/fsuid together and drops
 * capabilities by hand. That is a materially different scenario --
 * open(2) uses the *effective* uid, so it never exercises the real-uid
 * override at all -- and cannot reproduce this test's actual regression
 * coverage regardless of which uid value is passed in.
 *
 * A second, non-upstream case checks the contrasting scenario: a genuine
 * stranger (real uid changed, not just effective) gets no override and is
 * refused everything.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G088_ROOT	XFS_MNT "/g088"
#define G088_FILE	G088_ROOT "/t_access"

static void g088_creds_action(void *unused)
{
	xfs_restore_creds();
}

static void g088_remove_tree(void *unused)
{
	xfs_unlink(G088_FILE);
	xfs_rmdir(G088_ROOT);
}

/* the exact matrix and golden results from generic/088.out */
static const struct {
	int		mode;
	bool		allowed;
} g088_matrix[] = {
	{ 0,				true },  /* F_OK */
	{ MAY_READ,			true },  /* R_OK */
	{ MAY_WRITE,			true },  /* W_OK */
	{ MAY_EXEC,			false }, /* X_OK */
	{ MAY_READ | MAY_WRITE,		true },
	{ MAY_READ | MAY_EXEC,		false },
	{ MAY_WRITE | MAY_EXEC,		false },
	{ MAY_READ | MAY_WRITE | MAY_EXEC, false },
};

static void dac_override_follows_the_real_uid(struct kunit *test)
{
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G088_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g088_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g088_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G088_FILE, "x", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chown(G088_FILE, 500, 100), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G088_FILE, 0000), 0);

	/* seteuid(500): effective/fsuid move, real uid stays 0 */
	KUNIT_ASSERT_EQ(test, xfs_seteuid(500), 0);

	for (i = 0; i < ARRAY_SIZE(g088_matrix); i++) {
		int err = xfs_access(G088_FILE, g088_matrix[i].mode);

		if (g088_matrix[i].allowed)
			KUNIT_EXPECT_EQ_MSG(test, err, 0,
					    "mode 0x%x: expected allowed, got %d",
					    g088_matrix[i].mode, err);
		else
			KUNIT_EXPECT_EQ_MSG(test, err, -EACCES,
					    "mode 0x%x: expected EACCES, got %d",
					    g088_matrix[i].mode, err);
	}
	xfs_restore_creds();

	/*
	 * Contrast: a genuine stranger (real uid moved) has no override.
	 * F_OK (mode 0) is excluded -- it only checks existence, which
	 * nothing in this test ever denies, for anyone.
	 */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(99, 99), 0);
	KUNIT_EXPECT_EQ(test, xfs_access(G088_FILE, 0), 0);
	for (i = 1; i < ARRAY_SIZE(g088_matrix); i++) {
		int err = xfs_access(G088_FILE, g088_matrix[i].mode);

		KUNIT_EXPECT_EQ_MSG(test, err, -EACCES,
				    "stranger, mode 0x%x: expected EACCES, got %d",
				    g088_matrix[i].mode, err);
	}
	xfs_restore_creds();
}

static int g088_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g088_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g088_cases[] = {
	KUNIT_CASE(dac_override_follows_the_real_uid),
	{}
};

static struct kunit_suite g088_suite = {
	.name		= "xfstests/generic/088",
	.suite_init	= g088_suite_init,
	.suite_exit	= g088_suite_exit,
	.test_cases	= g088_cases,
};

kunit_test_suites(&g088_suite);

MODULE_DESCRIPTION("xfstests generic/088 over a loopback NFS mount");
MODULE_LICENSE("GPL");
