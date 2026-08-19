// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/088 over a loopback NFS mount: root and mode 000.
 *
 * Upstream's t_access: CAP_DAC_OVERRIDE handling. With no_root_squash,
 * root's identity reaches the server intact, so root opens a mode-000
 * file over NFS while an unprivileged identity is refused -- the ACCESS
 * RPC and the server's own check both have to agree.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G088_ROOT	XFS_MNT "/g088"

static void g088_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G088_FILE	G088_ROOT "/t_access"

static void g088_remove_tree(void *unused)
{
	xfs_unlink(G088_FILE);
	xfs_rmdir(G088_ROOT);
}

static void dac_override_is_roots_alone(struct kunit *test)
{
	struct file *f;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G088_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G088_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g088_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g088_creds_action, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G088_FILE, "x", 1), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G088_FILE, 0000), 0);

	/* root (no_root_squash): DAC override carries over the wire */
	f = filp_open(G088_FILE, O_RDWR, 0);
	KUNIT_EXPECT_FALSE_MSG(test, IS_ERR(f),
			       "root refused on a mode-000 file: %ld",
			       IS_ERR(f) ? PTR_ERR(f) : 0);
	if (!IS_ERR(f))
		filp_close(f, NULL);

	/* an unprivileged identity has no such override */
	KUNIT_ASSERT_EQ(test, xfs_switch_creds(99, 99), 0);
	f = filp_open(G088_FILE, O_RDONLY, 0);
	xfs_restore_creds();
	KUNIT_ASSERT_TRUE(test, IS_ERR(f));
	KUNIT_EXPECT_EQ_MSG(test, PTR_ERR(f), (long)-EACCES,
			    "expected EACCES for uid 99, got %ld", PTR_ERR(f));
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
	KUNIT_CASE(dac_override_is_roots_alone),
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
