// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/683 over a loopback NFS mount: fallocate by an
 * unprivileged user clears suid and sgid; by root it does not.
 *
 * Upstream writes 192K as root, sets one of six modes, runs "xfs_io -c
 * 'falloc 64k 64k'" either as qa_user or as root, and prints the mode
 * before and after. Its golden output encodes the rule: the unprivileged
 * caller, who is not in the file's group, loses S_ISUID and S_ISGID
 * whatever the exec bits; root keeps both.
 *
 * Over NFSv4.2 the operation is an ALLOCATE RPC (nfs42_proc_allocate()).
 * The stripping is the server's: shmem_fallocate() calls file_modified()
 * under the credentials knfsd took from the RPC. The client only learns
 * the new mode because _nfs42_proc_fallocate() marks it invalid when
 * nfs_should_remove_suid() says so. The port checks the server's mode
 * through the tmpfs export and the client's after a forced revalidation,
 * as the generic/355 port does.
 *
 * Deviations: qa_user is uid/gid 1000 with capabilities dropped
 * (xfs_switch_creds()); the file is root:root.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G683_ROOT	XFS_MNT "/g683"
#define G683_FILE	G683_ROOT "/a"
#define G683_SERVER	XFS_EXPORT "/g683/a"

#define G683_UID	1000
#define G683_GID	1000
#define G683_SIZE	(192 * 1024)
#define G683_MODE	(0)

/* upstream's ten cases, before and after modes from 683.out */
static const struct g683_case {
	const char	*name;
	bool		as_user;
	umode_t		before;
	umode_t		after;
} g683_table[] = {
	{ "1 - qa_user, non-exec file",			true,	06666,	00666 },
	{ "2 - qa_user, group-exec file",		true,	06676,	00676 },
	{ "3 - qa_user, user-exec file",		true,	06766,	00766 },
	{ "4 - qa_user, all-exec file",			true,	06777,	00777 },
	{ "5 - root, non-exec file",			false,	06666,	06666 },
	{ "6 - root, group-exec file",			false,	06676,	06676 },
	{ "7 - root, user-exec file",			false,	06766,	06766 },
	{ "8 - root, all-exec file",			false,	06777,	06777 },
	{ "9 - qa_user, group-exec file, only sgid",	true,	02676,	00676 },
	{ "10 - qa_user, all-exec file, only sgid",	true,	02777,	00777 },
};

static void g683_remove_tree(void *unused)
{
	xfs_restore_creds();
	xfs_unlink(G683_FILE);
	xfs_rmdir_settled(G683_ROOT);
}

static void g683_setup_testfile(struct kunit *test, u8 *buf)
{
	xfs_unlink(G683_FILE);
	xfs_settle_fput();
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G683_FILE, buf, G683_SIZE),
			0);
}

static void unprivileged_falloc_clears_suid_and_sgid(struct kunit *test)
{
	struct kstat st;
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G683_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g683_remove_tree, NULL),
			0);
	/* chmod a+rw $junk_dir */
	KUNIT_ASSERT_EQ(test, xfs_chmod(G683_ROOT, 0777), 0);

	buf = kunit_kmalloc(test, G683_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0x58, G683_SIZE);

	for (i = 0; i < ARRAY_SIZE(g683_table); i++) {
		const struct g683_case *c = &g683_table[i];
		struct file *f;
		int err;

		g683_setup_testfile(test, buf);
		KUNIT_ASSERT_EQ_MSG(test, xfs_chmod(G683_FILE, c->before), 0,
				    "%s: chmod %o", c->name, c->before);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G683_FILE, &st), 0);
		KUNIT_ASSERT_EQ_MSG(test, st.mode & 07777, c->before,
				    "%s: mode before is %o", c->name,
				    st.mode & 07777);

		if (c->as_user)
			KUNIT_ASSERT_EQ(test,
					xfs_switch_creds(G683_UID, G683_GID),
					0);
		f = filp_open(G683_FILE, O_RDWR, 0);
		if (IS_ERR(f)) {
			xfs_restore_creds();
			KUNIT_FAIL(test, "%s: open: %ld", c->name, PTR_ERR(f));
			return;
		}
		err = vfs_fallocate(f, G683_MODE, 65536, 65536);
		filp_close(f, NULL);
		xfs_restore_creds();
		KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: falloc: %d", c->name,
				    err);

		KUNIT_ASSERT_EQ(test, xfs_kstat(G683_SERVER, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 07777, c->after,
				    "%s: the server left the mode at %o, expected %o",
				    c->name, st.mode & 07777, c->after);
		KUNIT_ASSERT_EQ(test, xfs_kstat(G683_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 07777, c->after,
				    "%s: the client reports mode %o, expected %o",
				    c->name, st.mode & 07777, c->after);
	}
}

static int g683_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g683_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g683_cases[] = {
	KUNIT_CASE(unprivileged_falloc_clears_suid_and_sgid),
	{}
};

static struct kunit_suite g683_suite = {
	.name		= "xfstests/generic/683",
	.suite_init	= g683_suite_init,
	.suite_exit	= g683_suite_exit,
	.test_cases	= g683_cases,
};

kunit_test_suites(&g683_suite);

MODULE_DESCRIPTION("xfstests generic/683 over a loopback NFS mount");
MODULE_LICENSE("GPL");
