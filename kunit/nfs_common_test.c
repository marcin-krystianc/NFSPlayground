// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NFS status <-> errno translation in
 * fs/nfs_common/common.c.
 *
 * Three exported, pure table lookups shared by the NFS client and server:
 *
 *   nfs_stat_to_errno()            NFSv2/v3 status  -> errno
 *   nfs4_stat_to_errno()           NFSv4 status     -> errno
 *   nfs_localio_errno_to_nfs4_stat()  errno         -> NFSv4 status
 *
 * These decide which errno an application ultimately sees for a given
 * server response, so a wrong entry is a silent, protocol-visible bug.
 */

#include <kunit/test.h>

#include <linux/errno.h>
#include <linux/nfs_common.h>
#include <linux/nfs4.h>

struct stat_errno_param {
	const char	*desc;
	int		stat;
	int		errno;
};

static void stat_errno_get_desc(const struct stat_errno_param *param,
				char *desc)
{
	strscpy(desc, param->desc, KUNIT_PARAM_DESC_SIZE);
}

/*
 * nfs_stat_to_errno() -- NFSv2/v3
 */

static const struct stat_errno_param nfs_stat_params[] = {
	{ "NFS_OK",		NFS_OK,			0 },
	{ "NFSERR_PERM",	NFSERR_PERM,		-EPERM },
	{ "NFSERR_NOENT",	NFSERR_NOENT,		-ENOENT },
	{ "NFSERR_IO",		NFSERR_IO,		-EIO },
	{ "NFSERR_ACCES",	NFSERR_ACCES,		-EACCES },
	{ "NFSERR_EXIST",	NFSERR_EXIST,		-EEXIST },
	{ "NFSERR_NOTDIR",	NFSERR_NOTDIR,		-ENOTDIR },
	{ "NFSERR_ISDIR",	NFSERR_ISDIR,		-EISDIR },
	{ "NFSERR_FBIG",	NFSERR_FBIG,		-EFBIG },
	{ "NFSERR_NOSPC",	NFSERR_NOSPC,		-ENOSPC },
	{ "NFSERR_ROFS",	NFSERR_ROFS,		-EROFS },
	{ "NFSERR_NAMETOOLONG",	NFSERR_NAMETOOLONG,	-ENAMETOOLONG },
	{ "NFSERR_NOTEMPTY",	NFSERR_NOTEMPTY,	-ENOTEMPTY },
	{ "NFSERR_DQUOT",	NFSERR_DQUOT,		-EDQUOT },
	{ "NFSERR_STALE",	NFSERR_STALE,		-ESTALE },
	{ "NFSERR_BADHANDLE",	NFSERR_BADHANDLE,	-EBADHANDLE },
	{ "NFSERR_NOTSUPP",	NFSERR_NOTSUPP,		-ENOTSUPP },
	{ "NFSERR_TOOSMALL",	NFSERR_TOOSMALL,	-ETOOSMALL },
	{ "NFSERR_BADTYPE",	NFSERR_BADTYPE,		-EBADTYPE },
	{ "NFSERR_JUKEBOX",	NFSERR_JUKEBOX,		-EJUKEBOX },
	/* Deliberately mapped to -EREMOTEIO rather than -EIO. */
	{ "NFSERR_SERVERFAULT",	NFSERR_SERVERFAULT,	-EREMOTEIO },
};

KUNIT_ARRAY_PARAM(nfs_stat, nfs_stat_params, stat_errno_get_desc);

static void nfs_stat_to_errno_case(struct kunit *test)
{
	const struct stat_errno_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, nfs_stat_to_errno(param->stat),
			    param->errno, "status %d", param->stat);
}

/* An unrecognised status degrades to -EIO (common.c:65). */
static void nfs_stat_to_errno_unknown_is_eio(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs_stat_to_errno(999999), -EIO);
}

/*
 * NFSERR_EAGAIN is present in the protocol enum but commented out of the
 * table, so it must fall through to the -EIO default rather than mapping
 * to -EAGAIN. Pinning this stops it being "fixed" by accident.
 */
static void nfs_stat_to_errno_eagain_not_mapped(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs_stat_to_errno(NFSERR_EAGAIN), -EIO);
}

/*
 * nfs4_stat_to_errno() -- NFSv4
 */

static const struct stat_errno_param nfs4_stat_params[] = {
	/* from nfs4_errtbl_common */
	{ "NFS4_OK",		NFS4_OK,		0 },
	{ "NFS4ERR_PERM",	NFS4ERR_PERM,		-EPERM },
	{ "NFS4ERR_NOENT",	NFS4ERR_NOENT,		-ENOENT },
	{ "NFS4ERR_IO",		NFS4ERR_IO,		-EIO },
	{ "NFS4ERR_ACCESS",	NFS4ERR_ACCESS,		-EACCES },
	{ "NFS4ERR_EXIST",	NFS4ERR_EXIST,		-EEXIST },
	{ "NFS4ERR_NOTDIR",	NFS4ERR_NOTDIR,		-ENOTDIR },
	{ "NFS4ERR_ISDIR",	NFS4ERR_ISDIR,		-EISDIR },
	{ "NFS4ERR_FBIG",	NFS4ERR_FBIG,		-EFBIG },
	{ "NFS4ERR_NOSPC",	NFS4ERR_NOSPC,		-ENOSPC },
	{ "NFS4ERR_STALE",	NFS4ERR_STALE,		-ESTALE },
	{ "NFS4ERR_NAMETOOLONG", NFS4ERR_NAMETOOLONG,	-ENAMETOOLONG },
	{ "NFS4ERR_SYMLINK",	NFS4ERR_SYMLINK,	-ELOOP },
	{ "NFS4ERR_DEADLOCK",	NFS4ERR_DEADLOCK,	-EDEADLK },
	/* from nfs4_errtbl, consulted only after the common table */
	{ "NFS4ERR_SERVERFAULT", NFS4ERR_SERVERFAULT,	-EREMOTEIO },
	{ "NFS4ERR_LOCKED",	NFS4ERR_LOCKED,		-EAGAIN },
	{ "NFS4ERR_OP_ILLEGAL",	NFS4ERR_OP_ILLEGAL,	-EOPNOTSUPP },
	{ "NFS4ERR_NOXATTR",	NFS4ERR_NOXATTR,	-ENODATA },
	{ "NFS4ERR_XATTR2BIG",	NFS4ERR_XATTR2BIG,	-E2BIG },
};

KUNIT_ARRAY_PARAM(nfs4_stat, nfs4_stat_params, stat_errno_get_desc);

static void nfs4_stat_to_errno_case(struct kunit *test)
{
	const struct stat_errno_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, nfs4_stat_to_errno(param->stat),
			    param->errno, "status %d", param->stat);
}

/*
 * Untranslated NFSv4 codes are passed through as -stat, but only inside
 * the window 10000 < stat <= 10100. Outside it the server is considered
 * to be talking nonsense and -EREMOTEIO is returned (common.c:137-146).
 * These four cases pin both edges of that window.
 */
static void nfs4_stat_window_lower_edge_rejected(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(10000), -EREMOTEIO);
}

/*
 * NFS4ERR_SAME is inside the window and absent from both tables, so it is
 * returned untranslated. Note 10001 would NOT work here: that is
 * NFS4ERR_BADHANDLE, which the common table maps to -EBADHANDLE.
 */
static void nfs4_stat_window_passes_untabled_code_through(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(NFS4ERR_SAME),
			-NFS4ERR_SAME);
}

static void nfs4_stat_window_upper_edge_passed_through(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(10100), -10100);
}

static void nfs4_stat_window_upper_edge_rejected(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(10101), -EREMOTEIO);
}

static void nfs4_stat_negative_is_rejected(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(-1), -EREMOTEIO);
}

/*
 * nfs_localio_errno_to_nfs4_stat() -- reverse direction
 */

static const struct stat_errno_param localio_params[] = {
	/* resolved from nfs4_errtbl_common */
	{ "0 to NFS4_OK",		NFS4_OK,		0 },
	{ "EPERM to PERM",		NFS4ERR_PERM,		-EPERM },
	{ "ENOENT to NOENT",		NFS4ERR_NOENT,		-ENOENT },
	{ "EIO to IO",			NFS4ERR_IO,		-EIO },
	{ "EACCES to ACCESS",		NFS4ERR_ACCESS,		-EACCES },
	{ "ENOSPC to NOSPC",		NFS4ERR_NOSPC,		-ENOSPC },
	{ "ESTALE to STALE",		NFS4ERR_STALE,		-ESTALE },
	/* resolved from nfs4_errtbl_localio */
	{ "EREMOTEIO to IO",		NFS4ERR_IO,		-EREMOTEIO },
	{ "E2BIG to FBIG",		NFS4ERR_FBIG,		-E2BIG },
	{ "EBADF to STALE",		NFS4ERR_STALE,		-EBADF },
	{ "ETIMEDOUT to DELAY",		NFS4ERR_DELAY,		-ETIMEDOUT },
	{ "ENOMEM to DELAY",		NFS4ERR_DELAY,		-ENOMEM },
	{ "EBUSY to IO",		NFS4ERR_IO,		-EBUSY },
	{ "ENFILE to SERVERFAULT",	NFS4ERR_SERVERFAULT,	-ENFILE },
	{ "ENOKEY to PERM",		NFS4ERR_PERM,		-ENOKEY },
};

KUNIT_ARRAY_PARAM(localio, localio_params, stat_errno_get_desc);

static void localio_errno_to_stat_case(struct kunit *test)
{
	const struct stat_errno_param *param = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test,
			    nfs_localio_errno_to_nfs4_stat(param->errno),
			    (__u32)param->stat, "errno %d", param->errno);
}

static void localio_unknown_errno_is_serverfault(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs_localio_errno_to_nfs4_stat(-999999),
			(__u32)NFS4ERR_SERVERFAULT);
}

/*
 * The reverse lookup consults nfs4_errtbl_common then
 * nfs4_errtbl_localio, and never nfs4_errtbl. So -EAGAIN resolves to
 * NFS4ERR_DELAY via the localio table, not to NFS4ERR_LOCKED, which is
 * what the forward direction pairs it with.
 */
static void localio_eagain_prefers_delay_over_locked(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, nfs_localio_errno_to_nfs4_stat(-EAGAIN),
			(__u32)NFS4ERR_DELAY);
	KUNIT_EXPECT_EQ(test, nfs4_stat_to_errno(NFS4ERR_LOCKED), -EAGAIN);
}

/*
 * Round trips
 */

/* Codes in the common table survive stat -> errno -> stat. */
static void roundtrip_common_table_case(struct kunit *test)
{
	const struct stat_errno_param *param = test->param_value;
	int errno;

	/* Only the common-table entries are expected to round trip. */
	if (param->stat == NFS4ERR_SERVERFAULT ||
	    param->stat == NFS4ERR_LOCKED ||
	    param->stat == NFS4ERR_OP_ILLEGAL ||
	    param->stat == NFS4ERR_NOXATTR ||
	    param->stat == NFS4ERR_XATTR2BIG)
		kunit_skip(test, "not in nfs4_errtbl_common");

	errno = nfs4_stat_to_errno(param->stat);
	KUNIT_EXPECT_EQ_MSG(test, nfs_localio_errno_to_nfs4_stat(errno),
			    (__u32)param->stat,
			    "status %d did not round trip", param->stat);
}

/*
 * Not every code round trips, and that is deliberate: nfs4_errtbl_localio
 * exists precisely to "map errors differently than nfs4_errtbl".
 * NFS4ERR_SERVERFAULT converts to -EREMOTEIO, which converts back to
 * NFS4ERR_IO. Asserting the asymmetry documents it rather than leaving it
 * to be discovered.
 */
static void roundtrip_serverfault_is_asymmetric(struct kunit *test)
{
	int errno = nfs4_stat_to_errno(NFS4ERR_SERVERFAULT);

	KUNIT_EXPECT_EQ(test, errno, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, nfs_localio_errno_to_nfs4_stat(errno),
			(__u32)NFS4ERR_IO);
}

/*
 * Suites
 */

static struct kunit_case nfs_errno_v23_cases[] = {
	{
		.name			= "map NFSv2/v3 status",
		.run_case		= nfs_stat_to_errno_case,
		.generate_params	= nfs_stat_gen_params,
	},
	KUNIT_CASE(nfs_stat_to_errno_unknown_is_eio),
	KUNIT_CASE(nfs_stat_to_errno_eagain_not_mapped),
	{}
};

static struct kunit_suite nfs_errno_v23_suite = {
	.name		= "nfs-errno-v23",
	.test_cases	= nfs_errno_v23_cases,
};

static struct kunit_case nfs_errno_v4_cases[] = {
	{
		.name			= "map NFSv4 status",
		.run_case		= nfs4_stat_to_errno_case,
		.generate_params	= nfs4_stat_gen_params,
	},
	KUNIT_CASE(nfs4_stat_window_lower_edge_rejected),
	KUNIT_CASE(nfs4_stat_window_passes_untabled_code_through),
	KUNIT_CASE(nfs4_stat_window_upper_edge_passed_through),
	KUNIT_CASE(nfs4_stat_window_upper_edge_rejected),
	KUNIT_CASE(nfs4_stat_negative_is_rejected),
	{}
};

static struct kunit_suite nfs_errno_v4_suite = {
	.name		= "nfs-errno-v4",
	.test_cases	= nfs_errno_v4_cases,
};

static struct kunit_case nfs_errno_localio_cases[] = {
	{
		.name			= "map errno to NFSv4 status",
		.run_case		= localio_errno_to_stat_case,
		.generate_params	= localio_gen_params,
	},
	KUNIT_CASE(localio_unknown_errno_is_serverfault),
	KUNIT_CASE(localio_eagain_prefers_delay_over_locked),
	{}
};

static struct kunit_suite nfs_errno_localio_suite = {
	.name		= "nfs-errno-localio",
	.test_cases	= nfs_errno_localio_cases,
};

static struct kunit_case nfs_errno_roundtrip_cases[] = {
	{
		.name			= "status round trip",
		.run_case		= roundtrip_common_table_case,
		.generate_params	= nfs4_stat_gen_params,
	},
	KUNIT_CASE(roundtrip_serverfault_is_asymmetric),
	{}
};

static struct kunit_suite nfs_errno_roundtrip_suite = {
	.name		= "nfs-errno-roundtrip",
	.test_cases	= nfs_errno_roundtrip_cases,
};

kunit_test_suites(&nfs_errno_v23_suite,
		  &nfs_errno_v4_suite,
		  &nfs_errno_localio_suite,
		  &nfs_errno_roundtrip_suite);

MODULE_DESCRIPTION("Test NFS status to errno translation");
MODULE_LICENSE("GPL");
