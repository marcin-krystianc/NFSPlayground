// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/026 over a loopback NFS mount: ACL count limits.
 *
 * Upstream builds an access ACL with as many entries as the filesystem
 * claims to support (_acl_get_max), checks it applies, then checks one
 * entry beyond the limit is refused. It runs on POSIX ACLs via setfacl.
 *
 * NFSv4 has no POSIX ACL support: fs/nfs registers only the
 * system.nfs4_acl / nfs4_dacl / nfs4_sacl xattr handlers
 * (nfs4_xattr_handlers[], fs/nfs/nfs4proc.c:10988) and no
 * ->get_inode_acl / ->set_acl, so system.posix_acl_access has no handler
 * to reach. That is upstream's notrun reason, and the first thing this
 * port pins -- but pins as a *contract*, not as a proof of mechanism.
 * Established by mutation: giving the nfs4_acl handler the POSIX ACL name
 * so the call does reach the wire leaves this test passing, because the
 * server refuses with the same EOPNOTSUPP. On this deployment the errno
 * cannot separate "no client handler" from "server declined", and the
 * assertions below deliberately do not claim otherwise. What they do
 * establish is what a user sees: POSIX ACLs are unusable over this mount,
 * and a refused attempt changes nothing.
 *
 * The second half is the interesting one: the client DOES implement the
 * NFSv4 ACL xattr, so the protocol path exists. What it reaches on this
 * deployment is knfsd on tmpfs, and tmpfs is built here without
 * CONFIG_TMPFS_POSIX_ACL -- so the server has nothing to map an NFSv4 ACL
 * onto -- measured as EOPNOTSUPP for both GETACL and SETACL. The port
 * reports the errno rather than pinning it hard (a server with ACL support
 * would legitimately answer differently) and asserts the invariant that
 * matters on any server: whatever the answer, it is a clean refusal and
 * not a partial success, with the file mode untouched. Upstream's
 * count-limit logic stays unreachable until an ACL-carrying export exists.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G_ROOT		XFS_MNT "/g026"

#define G_FILE		G_ROOT "/aclfile"

/* a minimal POSIX access ACL blob: version + one USER_OBJ entry */
static const u8 g_posix_acl[] = {
	0x02, 0x00, 0x00, 0x00,			/* POSIX_ACL_XATTR_VERSION */
	0x01, 0x00, 0x06, 0x00,			/* USER_OBJ, perm rw- */
	0xff, 0xff, 0xff, 0xff,
};

static void g_remove_tree(void *unused)
{
	xfs_unlink(G_FILE);
	xfs_rmdir(G_ROOT);
}

/*
 * The NFSv4 ACL xattr, folded into the same case: a per-case cleanup
 * action removes the file at the end of its own case, so a second case
 * would find nothing there.
 */
static void g_nfsv4_acl_part(struct kunit *test)
{
	u8 *buf;
	ssize_t n;
	int err;

	buf = kunit_kzalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/*
	 * Measured on this deployment: EOPNOTSUPP, not ENODATA. The client
	 * implements the xattr, but knfsd is exporting tmpfs built without
	 * CONFIG_TMPFS_POSIX_ACL, so there is nothing for an NFSv4 ACL to
	 * map onto and the server says so. A positive length here would mean
	 * the export grew ACL support and upstream's count-limit logic
	 * becomes portable.
	 */
	n = xfs_getxattr(G_FILE, "system.nfs4_acl", buf, 4096);
	kunit_info(test, "GETACL (system.nfs4_acl) returned %zd", n);
	KUNIT_EXPECT_TRUE_MSG(test, n > 0 || n == -EOPNOTSUPP ||
			      n == -ENODATA || n == -EINVAL,
			      "GETACL gave an unexpected result: %zd", n);

	/* a deliberately malformed ACL: must be refused, never half-applied */
	err = xfs_setxattr(G_FILE, "system.nfs4_acl", buf, 8, 0);
	kunit_info(test, "SETACL with a malformed 8-byte ACL returned %d", err);
	KUNIT_EXPECT_LT_MSG(test, err, 0,
			    "a malformed NFSv4 ACL was accepted");

	{
		struct kstat st;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 0777, 0644,
				    "a refused SETACL changed the mode to %o",
				    st.mode & 0777);
	}
}

static void posix_acls_have_no_nfsv4_mapping(struct kunit *test)
{
	u8 buf[256];
	int err;
	ssize_t n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g_remove_tree, NULL), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G_FILE, "a", 1), 0);

	/* no handler is registered for the POSIX ACL names at all */
	err = xfs_setxattr(G_FILE, XATTR_NAME_POSIX_ACL_ACCESS,
			   g_posix_acl, sizeof(g_posix_acl), 0);
	KUNIT_EXPECT_EQ_MSG(test, err, -EOPNOTSUPP,
			    "setting a POSIX access ACL over NFS returned %d, expected EOPNOTSUPP",
			    err);
	err = xfs_setxattr(G_FILE, XATTR_NAME_POSIX_ACL_DEFAULT,
			   g_posix_acl, sizeof(g_posix_acl), 0);
	KUNIT_EXPECT_EQ(test, err, -EOPNOTSUPP);

	n = xfs_getxattr(G_FILE, XATTR_NAME_POSIX_ACL_ACCESS, buf,
			 sizeof(buf));
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)-EOPNOTSUPP,
			    "reading a POSIX access ACL over NFS returned %zd",
			    n);

	/* and the mode is untouched by the refused attempts */
	{
		struct kstat st;

		KUNIT_ASSERT_EQ(test, xfs_kstat(G_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.mode & 0777, 0644,
				    "a refused ACL set changed the mode to %o",
				    st.mode & 0777);
	}

	g_nfsv4_acl_part(test);
}

static int g_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g_cases[] = {
	KUNIT_CASE(posix_acls_have_no_nfsv4_mapping),
	{}
};

static struct kunit_suite g_suite = {
	.name		= "xfstests/generic/026",
	.suite_init	= g_suite_init,
	.suite_exit	= g_suite_exit,
	.test_cases	= g_cases,
};

kunit_test_suites(&g_suite);

MODULE_DESCRIPTION("xfstests generic/026 over a loopback NFS mount");
MODULE_LICENSE("GPL");
