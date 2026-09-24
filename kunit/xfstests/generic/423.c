// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/423 over a loopback NFS mount: statx of every kind of
 * object.
 *
 * Upstream creates a fifo, a character device, a directory, a block
 * device, a regular file, a symlink and an AF_UNIX socket, in that order,
 * and runs src/stat_test on each one with STATX_ALL:
 *
 *	ts_order	within the object, btime <= atime, btime <= mtime <=
 *			ctime (for the timestamps the filesystem reports)
 *	ref=<prev> ts=B,b ts=M,m
 *			its btime and mtime are not before the previously
 *			created object's
 *	stx_type, stx_mode, stx_rdev_major/minor, stx_size, stx_nlink
 *			the type, permissions, device numbers, size (20480
 *			for the file, the target's length for the symlink)
 *			and link count it was created with
 *
 * and finally hard-links the file: the link's btime lies between the
 * directory's and the socket's, its ctime is not before the socket's
 * btime or ctime, "cmp_ref" requires every statx field to equal the
 * file's own, and stx_nlink is 2.
 *
 * Over NFSv4 each of those is a separate attribute on the wire: type and
 * mode, rawdev, time_create, space_used... So the question is whether the
 * client decodes each into the right statx field for each object type,
 * including device numbers it never interprets itself.
 *
 * The socket is made with mknod(S_IFSOCK), which is what binding an
 * AF_UNIX socket to a path does (unix_bind() -> vfs_mknod()); the object
 * statx sees is the same.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>

#include "xfstests_nfs_fixture.h"

#define G423_ROOT	XFS_MNT "/g423"
#define G423_FIFO	G423_ROOT "/423-fifo"
#define G423_NULL	G423_ROOT "/423-null"
#define G423_DIR	G423_ROOT "/423-dir"
#define G423_LOOPY	G423_ROOT "/423-loopy"
#define G423_FILE	G423_ROOT "/423-file"
#define G423_SYMLINK	G423_ROOT "/423-symlink"
#define G423_TARGET	G423_ROOT "/423-nowhere"
#define G423_SOCK	G423_ROOT "/423-sock"
#define G423_LINK	G423_ROOT "/423-link"
#define G423_FILESZ	20480

static void g423_remove_tree(void *unused)
{
	xfs_unlink(G423_LINK);
	xfs_unlink(G423_SOCK);
	xfs_unlink(G423_SYMLINK);
	xfs_unlink(G423_FILE);
	xfs_unlink(G423_LOOPY);
	xfs_rmdir_settled(G423_DIR);
	xfs_unlink(G423_NULL);
	xfs_unlink(G423_FIFO);
	xfs_rmdir_settled(G423_ROOT);
}

/* statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_ALL) */
static int g423_statx(const char *path, struct kstat *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_getattr(&p, st, STATX_BASIC_STATS | STATX_BTIME,
			  AT_STATX_SYNC_AS_STAT);
	path_put(&p);
	return err;
}

/* check_earlier(): b is the same as or after a */
static void g423_earlier(struct kunit *test, const struct timespec64 *a,
			 const struct timespec64 *b, const char *an,
			 const char *bn, const char *what)
{
	KUNIT_EXPECT_TRUE_MSG(test,
			      b->tv_sec > a->tv_sec ||
			      (b->tv_sec == a->tv_sec && b->tv_nsec >= a->tv_nsec),
			      "%s: %s is before %s (%lld.%09ld < %lld.%09ld)",
			      what, bn, an, b->tv_sec, b->tv_nsec, a->tv_sec,
			      a->tv_nsec);
}

#define G423_HAS(st, m)	(((st)->result_mask & (m)) == (m))

/* ts_order */
static void g423_ts_order(struct kunit *test, const struct kstat *st,
			  const char *what)
{
	if (G423_HAS(st, STATX_BTIME | STATX_ATIME))
		g423_earlier(test, &st->btime, &st->atime, "btime", "atime", what);
	if (G423_HAS(st, STATX_BTIME | STATX_MTIME))
		g423_earlier(test, &st->btime, &st->mtime, "btime", "mtime", what);
	if (G423_HAS(st, STATX_BTIME | STATX_CTIME))
		g423_earlier(test, &st->btime, &st->ctime, "btime", "ctime", what);
	if (G423_HAS(st, STATX_MTIME | STATX_CTIME))
		g423_earlier(test, &st->mtime, &st->ctime, "mtime", "ctime", what);
}

/* ref=<prev> ts=B,b ts=M,m */
static void g423_after_ref(struct kunit *test, const struct kstat *ref,
			   const struct kstat *st, const char *what)
{
	if (G423_HAS(ref, STATX_BTIME) && G423_HAS(st, STATX_BTIME))
		g423_earlier(test, &ref->btime, &st->btime, "ref_b", "btime",
			     what);
	if (G423_HAS(ref, STATX_MTIME) && G423_HAS(st, STATX_MTIME))
		g423_earlier(test, &ref->mtime, &st->mtime, "ref_m", "mtime",
			     what);
}

/* stx_type, stx_mode, stx_rdev_major/minor, stx_nlink */
static void g423_fields(struct kunit *test, const struct kstat *st,
			umode_t type, int perm, unsigned int major,
			unsigned int minor, int nlink, const char *what)
{
	KUNIT_EXPECT_EQ_MSG(test, st->mode & S_IFMT, type, "%s: stx_type %o",
			    what, st->mode & S_IFMT);
	if (perm >= 0)
		KUNIT_EXPECT_EQ_MSG(test, st->mode & 07777, (umode_t)perm,
				    "%s: stx_mode %o", what, st->mode & 07777);
	KUNIT_EXPECT_EQ_MSG(test, MAJOR(st->rdev), major,
			    "%s: stx_rdev_major %u", what, MAJOR(st->rdev));
	KUNIT_EXPECT_EQ_MSG(test, MINOR(st->rdev), minor,
			    "%s: stx_rdev_minor %u", what, MINOR(st->rdev));
	if (nlink >= 0)
		KUNIT_EXPECT_EQ_MSG(test, st->nlink, (unsigned int)nlink,
				    "%s: stx_nlink %u", what, st->nlink);
}

/* cmp_ref: every statx field identical */
static void g423_cmp_ref(struct kunit *test, const struct kstat *st,
			 const struct kstat *ref)
{
#define G423_CMP(x)	KUNIT_EXPECT_EQ_MSG(test, st->x, ref->x, \
					    "attr '%s' differs from ref file", #x)
	G423_CMP(result_mask);
	G423_CMP(attributes);
	G423_CMP(blksize);
	G423_CMP(nlink);
	KUNIT_EXPECT_TRUE(test, uid_eq(st->uid, ref->uid));
	KUNIT_EXPECT_TRUE(test, gid_eq(st->gid, ref->gid));
	G423_CMP(mode);
	G423_CMP(ino);
	G423_CMP(size);
	G423_CMP(blocks);
	G423_CMP(atime.tv_sec);
	G423_CMP(atime.tv_nsec);
	G423_CMP(btime.tv_sec);
	G423_CMP(btime.tv_nsec);
	G423_CMP(ctime.tv_sec);
	G423_CMP(ctime.tv_nsec);
	G423_CMP(mtime.tv_sec);
	G423_CMP(mtime.tv_nsec);
	G423_CMP(rdev);
	G423_CMP(dev);
#undef G423_CMP
}

static void statx_reports_every_object_correctly(struct kunit *test)
{
	struct kstat fifo, chr, dir, blk, file, sym, sock, link;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G423_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g423_remove_tree, NULL),
			0);

	/* Test statx on a fifo: mkfifo -m 0600 */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_FIFO, S_IFIFO | 0600, 0, 0), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_FIFO, &fifo), 0);
	g423_ts_order(test, &fifo, "fifo");
	g423_fields(test, &fifo, S_IFIFO, 0600, 0, 0, 1, "fifo");

	/* Test statx on a chardev: mknod -m 0600 c 1 3 */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_NULL, S_IFCHR | 0600, 1, 3), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_NULL, &chr), 0);
	g423_ts_order(test, &chr, "chardev");
	g423_after_ref(test, &fifo, &chr, "chardev");
	g423_fields(test, &chr, S_IFCHR, 0600, 1, 3, 1, "chardev");

	/* Test statx on a directory */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G423_DIR), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_DIR, &dir), 0);
	g423_ts_order(test, &dir, "directory");
	g423_after_ref(test, &chr, &dir, "directory");
	g423_fields(test, &dir, S_IFDIR, 0755, 0, 0, -1, "directory");

	/* Test statx on a blockdev: mknod -m 0600 b 7 123 */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_LOOPY, S_IFBLK | 0600, 7, 123),
			0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_LOOPY, &blk), 0);
	g423_ts_order(test, &blk, "blockdev");
	g423_after_ref(test, &dir, &blk, "blockdev");
	g423_fields(test, &blk, S_IFBLK, 0600, 7, 123, 1, "blockdev");

	/* Test statx on a file: dd if=/dev/zero bs=1024 count=20 */
	buf = kunit_kzalloc(test, G423_FILESZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G423_FILE, buf, G423_FILESZ),
			0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_FILE, &file), 0);
	g423_ts_order(test, &file, "file");
	g423_after_ref(test, &blk, &file, "file");
	g423_fields(test, &file, S_IFREG, -1, 0, 0, 1, "file");
	KUNIT_EXPECT_EQ(test, file.size, (loff_t)G423_FILESZ);

	/* Test statx on a symlink */
	KUNIT_ASSERT_EQ(test, xfs_symlink(G423_TARGET, G423_SYMLINK), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_SYMLINK, &sym), 0);
	g423_ts_order(test, &sym, "symlink");
	g423_after_ref(test, &file, &sym, "symlink");
	g423_fields(test, &sym, S_IFLNK, -1, 0, 0, 1, "symlink");
	KUNIT_EXPECT_EQ(test, sym.size, (loff_t)strlen(G423_TARGET));

	/* Test statx on an AF_UNIX socket */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_SOCK, S_IFSOCK | 0755, 0, 0), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_SOCK, &sock), 0);
	g423_ts_order(test, &sock, "socket");
	g423_after_ref(test, &sym, &sock, "socket");
	g423_fields(test, &sock, S_IFSOCK, -1, 0, 0, 1, "socket");

	/* Test a hard link to a file */
	KUNIT_ASSERT_EQ(test, xfs_link(G423_FILE, G423_LINK), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_LINK, &link), 0);
	if (G423_HAS(&link, STATX_BTIME)) {
		/* ref=dir ts=B,b; ref=sock ts=b,B ts=B,c */
		g423_earlier(test, &dir.btime, &link.btime, "ref_b", "btime",
			     "link");
		g423_earlier(test, &link.btime, &sock.btime, "btime", "ref_b",
			     "link");
		g423_earlier(test, &sock.btime, &link.ctime, "ref_b", "ctime",
			     "link");
	}
	/* ts=C,c */
	g423_earlier(test, &sock.ctime, &link.ctime, "ref_c", "ctime", "link");
	/* ref=file cmp_ref */
	KUNIT_ASSERT_EQ(test, g423_statx(G423_FILE, &file), 0);
	g423_cmp_ref(test, &link, &file);
	KUNIT_EXPECT_EQ(test, link.nlink, 2U);
}

static int g423_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g423_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g423_cases[] = {
	KUNIT_CASE(statx_reports_every_object_correctly),
	{}
};

static struct kunit_suite g423_suite = {
	.name		= "xfstests/generic/423",
	.suite_init	= g423_suite_init,
	.suite_exit	= g423_suite_exit,
	.test_cases	= g423_cases,
};

kunit_test_suites(&g423_suite);

MODULE_DESCRIPTION("xfstests generic/423 over a loopback NFS mount");
MODULE_LICENSE("GPL");
