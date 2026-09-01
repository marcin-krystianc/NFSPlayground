// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/126 over a loopback NFS mount: the fs_perms read/write matrix.
 *
 * Upstream's src/fs_perms: set a file's mode and ownership, become
 * another identity, try an operation, compare against the expected
 * verdict. Over NFS the identity is the AUTH_SYS credential on the wire
 * and the verdict is the server's (plus the client's ACCESS cache), so
 * the whole owner/group/other x read/write matrix is protocol behaviour.
 * Execute rows are dropped: there is no exec from a kthread.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G126_ROOT	XFS_MNT "/g126"

static void g126_creds_action(void *unused)
{
	xfs_restore_creds();
}

#define G126_FILE	G126_ROOT "/testx.file"

static const struct g126_row {
	umode_t		mode;
	uid_t		fuid;	gid_t fgid;	/* file ownership */
	uid_t		uid;	gid_t gid;	/* acting identity */
	char		op;			/* r or w */
	bool		allowed;
} g126_rows[] = {
	{ 0600, 99, 99,  99,  99, 'r', true  },
	{ 0600, 99, 99,  99,  99, 'w', true  },
	{ 0600, 99, 99, 100,  99, 'r', false },
	{ 0600, 99, 99, 100,  99, 'w', false },
	{ 0060, 99, 99, 100,  99, 'r', true  },	/* group grants it */
	{ 0060, 99, 99, 100,  99, 'w', true  },
	{ 0060, 99, 99, 100, 500, 'r', false },	/* wrong group */
	{ 0006, 99, 99, 100, 500, 'r', true  },	/* other grants it */
	{ 0006, 99, 99, 100, 500, 'w', true  },
	{ 0004, 99, 99, 100, 500, 'w', false },	/* other: read only */
	{ 0400, 99, 99,  99,  99, 'w', false },	/* owner: read only */
};

static void g126_remove_tree(void *unused)
{
	xfs_unlink(G126_FILE);
	xfs_rmdir(G126_ROOT);
}

static void the_rw_matrix_matches_posix(struct kunit *test)
{
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G126_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G126_ROOT, 0777), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g126_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g126_creds_action, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(g126_rows); i++) {
		const struct g126_row *row = &g126_rows[i];
		struct file *f;
		bool ok;

		/* root sets the stage */
		xfs_unlink(G126_FILE);
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(G126_FILE, "data", 4), 0);
		KUNIT_ASSERT_EQ(test,
				xfs_chown(G126_FILE, row->fuid, row->fgid), 0);
		KUNIT_ASSERT_EQ(test, xfs_chmod(G126_FILE, row->mode), 0);

		/* ...someone else attempts the operation */
		KUNIT_ASSERT_EQ(test,
				xfs_switch_creds(row->uid, row->gid), 0);
		f = filp_open(G126_FILE,
			      row->op == 'r' ? O_RDONLY : O_WRONLY, 0);
		ok = !IS_ERR(f);
		if (ok)
			filp_close(f, NULL);
		xfs_restore_creds();

		KUNIT_EXPECT_EQ_MSG(test, ok, row->allowed,
				    "row %d: mode %o file %d:%d as %d:%d op %c -- got %s",
				    i, row->mode, row->fuid, row->fgid,
				    row->uid, row->gid, row->op,
				    ok ? "allowed" : "denied");
	}
}

static int g126_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g126_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g126_cases[] = {
	KUNIT_CASE(the_rw_matrix_matches_posix),
	{}
};

static struct kunit_suite g126_suite = {
	.name		= "xfstests/generic/126",
	.suite_init	= g126_suite_init,
	.suite_exit	= g126_suite_exit,
	.test_cases	= g126_cases,
};

kunit_test_suites(&g126_suite);

MODULE_DESCRIPTION("xfstests generic/126 over a loopback NFS mount");
MODULE_LICENSE("GPL");
