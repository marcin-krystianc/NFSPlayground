// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/025 over a loopback NFS mount: RENAME_EXCHANGE across
 * the full source/destination type matrix.
 *
 * Upstream 023, 024, 025 and 078 all call common/renameat2's
 * _rename_tests, differing only in the flag passed to src/renameat2:
 * none, -n (RENAME_NOREPLACE), -x (RENAME_EXCHANGE), -w
 * (RENAME_WHITEOUT). _rename_tests is not one rename -- it is a 5x5
 * matrix of source type x destination type (none, regular file, symlink,
 * empty directory, populated directory) run twice, once same-directory
 * and once cross-directory: fifty combinations per flag.
 *
 * generic/024 is already ported, but only as three single-combination
 * probes of the three flags. That leaves the matrix itself untested, and
 * the matrix is the substance of these tests -- the same lesson the
 * collapse family taught (see generic/012 and 016).
 *
 * The answer is not uniform, and finding that out is the point of running
 * the whole matrix. RENAME_EXCHANGE requires both names to exist, and the
 * VFS resolves them before any filesystem is consulted, so:
 *
 *   - either name absent -> ENOENT, from do_renameat2()'s own checks
 *                          ("source must exist", then "EXCHANGE needs an
 *                          existing target"), before any filesystem is
 *                          consulted
 *   - both present       -> EINVAL, from nfs_rename(), which rejects every
 *                          renameat2 flag because NFSv4 has no flagged
 *                          RENAME
 *
 * Eighteen of the fifty combinations therefore never reach the NFS client
 * at all -- which is itself worth pinning, since it means generic/024's
 * single EINVAL probe was only ever exercising one of the two regimes. This port derives the expected errno from the type pair and asserts
 * it, which is strictly more informative than the single EINVAL probe in
 * generic/024 -- it shows exactly where the VFS stops and where NFS stops.
 * It also asserts that not one of the fifty is disturbed by the attempt: a
 * partially-applied exchange would be a data-loss bug, and invisible to a
 * single-combination probe.
 *
 * Deviation: upstream prints the resulting types and diffs against a
 * golden file, which encodes per-filesystem behaviour where the flag is
 * supported. Here the flag is never supported, so the port asserts the
 * errno and the untouched state directly.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>

#include "internal.h"
#include "xfstests_nfs_fixture.h"

#define G025_ROOT	XFS_MNT "/g025"

/* upstream's _setup_one types, in its order */
enum g025_type {
	G025_NONE,
	G025_REGU,
	G025_SYMB,
	G025_DIRE,
	G025_TREE,
	G025_NTYPES
};

static const char * const g025_typename[G025_NTYPES] = {
	"none", "regu", "symb", "dire", "tree"
};

static int g025_rename_flags(const char *a, const char *b, unsigned int fl)
{
	return do_renameat2(AT_FDCWD, getname_kernel(a),
			    AT_FDCWD, getname_kernel(b), fl);
}

/* _setup_one */
static void g025_setup_one(struct kunit *test, const char *path,
			   enum g025_type type)
{
	char sub[96];

	switch (type) {
	case G025_NONE:
		break;
	case G025_REGU:
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_write_new_file(path, "foo\n", 4), 0,
				    "creating regular file %s", path);
		break;
	case G025_SYMB:
		KUNIT_ASSERT_EQ_MSG(test, xfs_symlink("foo", path), 0,
				    "creating symlink %s", path);
		break;
	case G025_DIRE:
		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(path), 0,
				    "creating directory %s", path);
		break;
	case G025_TREE:
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		snprintf(sub, sizeof(sub), "%s/bar", path);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(sub, "foo\n", 4), 0);
		break;
	default:
		break;
	}
}

/*
 * _cleanup_one. Unconditional and order-independent: just try every shape.
 *
 * The settled rmdir is load-bearing, and cost two debugging rounds to find.
 * Removing the "tree" type's child leaves a sillyrename (.nfsXXXX) entry
 * behind while that file's delayed fput is still pending, so a plain rmdir
 * returns ENOTEMPTY and the directory survives into the next iteration --
 * where it shows up as a bogus "dst should not exist but does" three
 * combinations later, or as EISDIR when the next layout tries to create a
 * regular file over it. The assertion below keeps that diagnosable: a
 * cleanup failure is reported here, not as a mystery elsewhere.
 */
static void g025_cleanup_one(struct kunit *test, const char *path)
{
	char sub[96];

	snprintf(sub, sizeof(sub), "%s/bar", path);
	xfs_unlink(sub);
	xfs_unlink(path);
	xfs_rmdir_settled(path);
	KUNIT_ASSERT_FALSE_MSG(test, xfs_exists(path),
			       "cleanup left %s behind", path);
}

/*
 * _showtype_one's job, reduced to what can be asserted: confirm the entry
 * still has exactly the type it was set up with. This is what catches a
 * partially-applied exchange.
 *
 * xfs_kstat() is lstat semantics, not stat: kern_path() only follows a
 * trailing symlink when LOOKUP_FOLLOW is passed (fs/namei.c:1906), and it
 * is not. So a symlink reports as one rather than as its (absent) target.
 */
static void g025_expect_type(struct kunit *test, const char *path,
			     enum g025_type type, const char *ctx)
{
	struct kstat st;
	char sub[96];
	int err;

	if (type == G025_NONE) {
		KUNIT_ASSERT_FALSE_MSG(test, xfs_exists(path),
				       "%s: %s should not exist but does", ctx,
				       path);
		return;
	}

	err = xfs_kstat(path, &st);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: stat %s failed (%d)", ctx, path,
			    err);

	switch (type) {
	case G025_REGU:
		KUNIT_ASSERT_TRUE_MSG(test, S_ISREG(st.mode),
				      "%s: %s is no longer a regular file (mode %o)",
				      ctx, path, st.mode);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)4,
				    "%s: %s content changed", ctx, path);
		break;
	case G025_SYMB:
		KUNIT_ASSERT_TRUE_MSG(test, S_ISLNK(st.mode),
				      "%s: %s is no longer a symlink (mode %o)",
				      ctx, path, st.mode);
		KUNIT_ASSERT_EQ_MSG(test, st.size, (loff_t)3,
				    "%s: %s target length changed to %lld",
				    ctx, path, st.size);
		break;
	case G025_DIRE:
		KUNIT_ASSERT_TRUE_MSG(test, S_ISDIR(st.mode),
				      "%s: %s is no longer a directory", ctx,
				      path);
		break;
	case G025_TREE:
		KUNIT_ASSERT_TRUE_MSG(test, S_ISDIR(st.mode),
				      "%s: %s is no longer a directory", ctx,
				      path);
		snprintf(sub, sizeof(sub), "%s/bar", path);
		KUNIT_ASSERT_TRUE_MSG(test, xfs_exists(sub),
				      "%s: %s lost its child", ctx, path);
		break;
	default:
		break;
	}
}

/* best-effort teardown: no struct kunit here, so nothing is asserted */
static void g025_scrub(const char *path)
{
	char sub[96];

	snprintf(sub, sizeof(sub), "%s/bar", path);
	xfs_unlink(sub);
	xfs_unlink(path);
	xfs_rmdir_settled(path);
}

static void g025_remove_tree(void *unused)
{
	g025_scrub(G025_ROOT "/src");
	g025_scrub(G025_ROOT "/dst");
	g025_scrub(G025_ROOT "/x/src");
	g025_scrub(G025_ROOT "/y/dst");
	xfs_rmdir(G025_ROOT "/x");
	xfs_rmdir(G025_ROOT "/y");
	xfs_rmdir_settled(G025_ROOT);
}

/* _rename_tests_source_dest */
static void g025_matrix(struct kunit *test, const char *src, const char *dst,
			const char *where)
{
	int s, d;

	for (s = 0; s < G025_NTYPES; s++) {
		for (d = 0; d < G025_NTYPES; d++) {
			char ctx[64];
			int err, expected;

			snprintf(ctx, sizeof(ctx), "%s %s/%s", where,
				 g025_typename[s], g025_typename[d]);

			g025_setup_one(test, src, s);
			g025_setup_one(test, dst, d);

			/*
			 * RENAME_EXCHANGE needs both names, and
			 * do_renameat2() resolves both before calling into
			 * the filesystem: "source must exist" first, then
			 * "EXCHANGE needs an existing target". Either one
			 * absent is ENOENT and never reaches NFS; only when
			 * both exist does the flag get to nfs_rename() and
			 * earn its EINVAL.
			 */
			expected = (s == G025_NONE || d == G025_NONE) ?
				   -ENOENT : -EINVAL;

			err = g025_rename_flags(src, dst, RENAME_EXCHANGE);
			KUNIT_ASSERT_EQ_MSG(test, err, expected,
					    "%s: RENAME_EXCHANGE returned %d, expected %d",
					    ctx, err, expected);

			/* nothing may have moved, in either direction */
			g025_expect_type(test, src, s, ctx);
			g025_expect_type(test, dst, d, ctx);

			g025_cleanup_one(test, src);
			g025_cleanup_one(test, dst);
		}
	}
}

static void exchange_is_refused_for_every_type_pairing(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G025_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g025_remove_tree,
						  NULL), 0);

	/* same-directory renames */
	g025_matrix(test, G025_ROOT "/src", G025_ROOT "/dst", "samedir ");

	/* cross-directory renames */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G025_ROOT "/x"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G025_ROOT "/y"), 0);
	g025_matrix(test, G025_ROOT "/x/src", G025_ROOT "/y/dst", "crossdir");
	KUNIT_EXPECT_EQ(test, xfs_rmdir(G025_ROOT "/x"), 0);
	KUNIT_EXPECT_EQ(test, xfs_rmdir(G025_ROOT "/y"), 0);
}

static int g025_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g025_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g025_cases[] = {
	KUNIT_CASE(exchange_is_refused_for_every_type_pairing),
	{}
};

static struct kunit_suite g025_suite = {
	.name		= "xfstests/generic/025",
	.suite_init	= g025_suite_init,
	.suite_exit	= g025_suite_exit,
	.test_cases	= g025_cases,
};

kunit_test_suites(&g025_suite);

MODULE_DESCRIPTION("xfstests generic/025 over a loopback NFS mount");
MODULE_LICENSE("GPL");
