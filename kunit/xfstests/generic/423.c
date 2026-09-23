// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/423 over a loopback NFS mount: statx of every kind of
 * object.
 *
 * Upstream creates a fifo, a character device, a directory, a block
 * device, a regular file, a symlink and an AF_UNIX socket in that order,
 * and after each one has src/stat_test check the type, mode, rdev, size
 * and link count it reports -- and that its btime and ctime are at or
 * after the previous object's ("ts_order"). It finishes with a hard link,
 * whose ctime must have moved while its btime must not.
 *
 * Over NFSv4 each of those attributes is a separate attribute on the
 * wire: type and mode come from the fattr4 bitmap, rdev from
 * rawdev, btime from time_create. So the port is really asking whether
 * the client decodes each one into the right statx field for each object
 * type -- including the two the client never creates itself (a block
 * device's major/minor round-tripping through the server).
 *
 * Deviations: the AF_UNIX socket is left out; creating one needs a bound
 * socket rather than a filesystem operation, and the other seven objects
 * cover the decode. Sizes are compared exactly, as upstream does, and
 * the timestamp ordering is checked with the full timespec.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/stat.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>

#include "xfstests_nfs_fixture.h"

#define G423_ROOT	XFS_MNT "/g423"
#define G423_FIFO	G423_ROOT "/423-fifo"
#define G423_NULL	G423_ROOT "/423-null"
#define G423_DIR	G423_ROOT "/423-dir"
#define G423_LOOPY	G423_ROOT "/423-loopy"
#define G423_FILE	G423_ROOT "/423-file"
#define G423_SYMLINK	G423_ROOT "/423-symlink"
#define G423_TARGET	G423_ROOT "/423-nowhere"
#define G423_LINK	G423_ROOT "/423-link"

#define G423_FILESZ	20480

static void g423_remove_tree(void *unused)
{
	xfs_unlink(G423_LINK);
	xfs_unlink(G423_SYMLINK);
	xfs_unlink(G423_FILE);
	xfs_unlink(G423_LOOPY);
	xfs_rmdir_settled(G423_DIR);
	xfs_unlink(G423_NULL);
	xfs_unlink(G423_FIFO);
	xfs_rmdir_settled(G423_ROOT);
}

/* statx with btime, without following a trailing symlink */
static int g423_statx(const char *path, struct kstat *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_getattr(&p, st, STATX_BASIC_STATS | STATX_BTIME,
			  AT_STATX_FORCE_SYNC);
	path_put(&p);
	return err;
}

static bool g423_not_before(const struct timespec64 *a,
			    const struct timespec64 *b)
{
	return a->tv_sec > b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec >= b->tv_nsec);
}

/* upstream's ts_order: this object's btime and ctime are not before the
 * previous object's
 */
static void g423_ts_order(struct kunit *test, const struct kstat *prev,
			  const struct kstat *st, const char *what)
{
	if (!(prev->result_mask & STATX_BTIME) ||
	    !(st->result_mask & STATX_BTIME))
		return;
	KUNIT_EXPECT_TRUE_MSG(test, g423_not_before(&st->btime, &prev->btime),
			      "%s: btime %lld.%09ld is before the previous object's %lld.%09ld",
			      what, st->btime.tv_sec, st->btime.tv_nsec,
			      prev->btime.tv_sec, prev->btime.tv_nsec);
	KUNIT_EXPECT_TRUE_MSG(test, g423_not_before(&st->ctime, &prev->ctime),
			      "%s: ctime %lld.%09ld is before the previous object's %lld.%09ld",
			      what, st->ctime.tv_sec, st->ctime.tv_nsec,
			      prev->ctime.tv_sec, prev->ctime.tv_nsec);
}

static void statx_reports_every_object_correctly(struct kunit *test)
{
	struct kstat fifo, chr, dir, blk, file, sym, link;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G423_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g423_remove_tree, NULL),
			0);

	/* a fifo */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_FIFO, S_IFIFO | 0600, 0, 0), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_FIFO, &fifo), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISFIFO(fifo.mode), "fifo: mode %o",
			      fifo.mode);
	KUNIT_EXPECT_EQ(test, fifo.mode & 07777, 0600);
	KUNIT_EXPECT_EQ(test, MAJOR(fifo.rdev), 0U);
	KUNIT_EXPECT_EQ(test, MINOR(fifo.rdev), 0U);
	KUNIT_EXPECT_EQ(test, fifo.nlink, 1U);

	msleep(20);

	/* a character device */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_NULL, S_IFCHR | 0600, 1, 3), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_NULL, &chr), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISCHR(chr.mode), "chardev: mode %o",
			      chr.mode);
	KUNIT_EXPECT_EQ(test, chr.mode & 07777, 0600);
	KUNIT_EXPECT_EQ_MSG(test, MAJOR(chr.rdev), 1U, "chardev: major %u",
			    MAJOR(chr.rdev));
	KUNIT_EXPECT_EQ_MSG(test, MINOR(chr.rdev), 3U, "chardev: minor %u",
			    MINOR(chr.rdev));
	KUNIT_EXPECT_EQ(test, chr.nlink, 1U);
	g423_ts_order(test, &fifo, &chr, "chardev");

	msleep(20);

	/* a directory */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G423_DIR), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_DIR, &dir), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISDIR(dir.mode), "dir: mode %o",
			      dir.mode);
	KUNIT_EXPECT_EQ(test, dir.mode & 07777, 0755);
	KUNIT_EXPECT_EQ(test, MAJOR(dir.rdev), 0U);
	g423_ts_order(test, &chr, &dir, "directory");

	msleep(20);

	/* a block device */
	KUNIT_ASSERT_EQ(test, xfs_mknod(G423_LOOPY, S_IFBLK | 0600, 7, 123),
			0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_LOOPY, &blk), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISBLK(blk.mode), "blockdev: mode %o",
			      blk.mode);
	KUNIT_EXPECT_EQ(test, blk.mode & 07777, 0600);
	KUNIT_EXPECT_EQ_MSG(test, MAJOR(blk.rdev), 7U, "blockdev: major %u",
			    MAJOR(blk.rdev));
	KUNIT_EXPECT_EQ_MSG(test, MINOR(blk.rdev), 123U, "blockdev: minor %u",
			    MINOR(blk.rdev));
	KUNIT_EXPECT_EQ(test, blk.nlink, 1U);
	g423_ts_order(test, &dir, &blk, "blockdev");

	msleep(20);

	/* a regular file of a known size */
	buf = kunit_kzalloc(test, G423_FILESZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G423_FILE, buf, G423_FILESZ),
			0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_FILE, &file), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISREG(file.mode), "file: mode %o",
			      file.mode);
	KUNIT_EXPECT_EQ_MSG(test, file.size, (loff_t)G423_FILESZ,
			    "file: size %lld", file.size);
	KUNIT_EXPECT_EQ(test, file.nlink, 1U);
	g423_ts_order(test, &blk, &file, "file");

	msleep(20);

	/* a symlink, whose size is the length of its target */
	KUNIT_ASSERT_EQ(test, xfs_symlink(G423_TARGET, G423_SYMLINK), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_SYMLINK, &sym), 0);
	KUNIT_EXPECT_TRUE_MSG(test, S_ISLNK(sym.mode), "symlink: mode %o",
			      sym.mode);
	KUNIT_EXPECT_EQ_MSG(test, sym.size, (loff_t)strlen(G423_TARGET),
			    "symlink: size %lld, expected %zu", sym.size,
			    strlen(G423_TARGET));
	KUNIT_EXPECT_EQ(test, sym.nlink, 1U);
	g423_ts_order(test, &file, &sym, "symlink");

	msleep(20);

	/* and a hard link: the target's ctime moves, its btime does not */
	KUNIT_ASSERT_EQ(test, xfs_link(G423_FILE, G423_LINK), 0);
	KUNIT_ASSERT_EQ(test, g423_statx(G423_LINK, &link), 0);
	KUNIT_EXPECT_EQ_MSG(test, link.nlink, 2U, "link: nlink %u",
			    link.nlink);
	KUNIT_EXPECT_TRUE_MSG(test, g423_not_before(&link.ctime, &sym.ctime),
			      "link: ctime did not move past the symlink's");
	if (file.result_mask & STATX_BTIME)
		KUNIT_EXPECT_EQ_MSG(test, link.btime.tv_sec, file.btime.tv_sec,
				    "link: btime moved from %lld to %lld",
				    file.btime.tv_sec, link.btime.tv_sec);
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
