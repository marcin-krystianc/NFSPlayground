// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/126 over a loopback NFS mount: the fs_perms matrix.
 *
 * Upstream's src/fs_perms, once per row: copy testx.file to test.file as
 * root, chmod and chown it, setegid/seteuid to the tester, and try the
 * operation -- fopen(3) with "r" or "w", or exec for "x". The eighteen
 * rows below are upstream's, in its order. generic/126.out prints PASS
 * for the first nine (the operation is allowed) and FAIL for the other
 * nine (it is refused), so that is the verdict each row asserts.
 *
 * Over NFS the identity is the AUTH_SYS credential on the wire and the
 * verdict is the server's plus the client's ACCESS cache, so the whole
 * owner/group/other x read/write/execute matrix is protocol behaviour.
 *
 * "r" and "w" open the file with fopen's flags (O_RDONLY, and O_WRONLY |
 * O_CREAT | O_TRUNC). "x" cannot exec from a kernel thread, so it calls
 * open_exec(), the part of execve(2) that decides whether the file may be
 * executed: the same MAY_EXEC permission check, the same ACCESS over NFS.
 * The tester's identity is set with fsuid/fsgid and no capabilities, which
 * is what seteuid/setegid away from root leave a process with.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/binfmts.h>

#include "xfstests_nfs_fixture.h"

#define G126_ROOT	XFS_MNT "/g126"
#define G126_FILE	G126_ROOT "/test.file"

/* $QA_FS_PERMS <mode> <file uid> <file gid> <uid> <gid> <r|w|x> 1 */
static const struct g126_row {
	umode_t		mode;
	uid_t		fuid;	gid_t fgid;
	uid_t		uid;	gid_t gid;
	char		op;
	bool		allowed;	/* PASS in 126.out */
} g126_rows[] = {
	{ 0001, 99, 99,  12, 100, 'x', true  },
	{ 0010, 99, 99, 200,  99, 'x', true  },
	{ 0100, 99, 99,  99, 500, 'x', true  },
	{ 0002, 99, 99,  12, 100, 'w', true  },
	{ 0020, 99, 99, 200,  99, 'w', true  },
	{ 0200, 99, 99,  99, 500, 'w', true  },
	{ 0004, 99, 99,  12, 100, 'r', true  },
	{ 0040, 99, 99, 200,  99, 'r', true  },
	{ 0400, 99, 99,  99, 500, 'r', true  },
	{ 0000, 99, 99,  99,  99, 'r', false },
	{ 0000, 99, 99,  99,  99, 'w', false },
	{ 0000, 99, 99,  99,  99, 'x', false },
	{ 0010, 99, 99,  99, 500, 'x', false },
	{ 0100, 99, 99, 200,  99, 'x', false },
	{ 0020, 99, 99,  99, 500, 'w', false },
	{ 0200, 99, 99, 200,  99, 'w', false },
	{ 0040, 99, 99,  99, 500, 'r', false },
	{ 0400, 99, 99, 200,  99, 'r', false },
};

static void g126_creds_action(void *unused)
{
	xfs_restore_creds();
}

static void g126_remove_tree(void *unused)
{
	xfs_unlink(G126_FILE);
	xfs_rmdir_settled(G126_ROOT);
}

/* testfperm(): 0 if the operation was allowed, else its errno */
static int g126_try(char op)
{
	struct file *f;

	switch (op) {
	case 'x':
		f = open_exec(G126_FILE);
		break;
	case 'w':
		f = filp_open(G126_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		break;
	default:
		f = filp_open(G126_FILE, O_RDONLY, 0);
		break;
	}
	if (IS_ERR(f))
		return PTR_ERR(f);
	fput(f);
	return 0;
}

static void the_fs_perms_matrix_matches_upstream(struct kunit *test)
{
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G126_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_chmod(G126_ROOT, 0755), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g126_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g126_creds_action, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(g126_rows); i++) {
		const struct g126_row *row = &g126_rows[i];
		int err;

		/* testsetup(): cp testx.file test.file; chmod; chown */
		xfs_unlink(G126_FILE);
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(G126_FILE, "\x7f" "ELF", 4),
				0);
		KUNIT_ASSERT_EQ(test, xfs_chmod(G126_FILE, row->mode), 0);
		KUNIT_ASSERT_EQ(test,
				xfs_chown(G126_FILE, row->fuid, row->fgid), 0);
		/* cp has exited: no write reference may linger (ETXTBSY) */
		xfs_settle_fput();

		KUNIT_ASSERT_EQ(test,
				xfs_switch_creds(row->uid, row->gid), 0);
		err = g126_try(row->op);
		xfs_restore_creds();

		KUNIT_EXPECT_EQ_MSG(test, !err, row->allowed,
				    "%c a %03o file owned by (%d/%d) as user/group(%d/%d): %d, expected %s",
				    row->op, row->mode, row->fuid, row->fgid,
				    row->uid, row->gid, err,
				    row->allowed ? "allowed" : "refused");
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
	KUNIT_CASE(the_fs_perms_matrix_matches_upstream),
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
