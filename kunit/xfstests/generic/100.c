// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/100 over a loopback NFS mount: copy a directory tree
 * onto the filesystem and compare it with the original.
 *
 * Upstream builds a tree with _populate_fs (-n 3 dirs, -f 6 files, -d 5
 * deep, 10k each) in /tmp, tars it up, untars it onto the test
 * filesystem, and requires "diff -qr" to find no difference.
 *
 * What that exercises over NFS is bulk namespace construction: a few
 * hundred CREATEs and MKDIRs interleaved with WRITEs, then a full walk of
 * the result. The port keeps that shape and replaces tar with a recursive
 * copy, since tar is the part that has nothing to do with the filesystem.
 * The source tree is built in the UML kernel's own root filesystem
 * (ramfs), which is the local filesystem standing in for upstream's /tmp.
 *
 * The comparison is stronger than diff -qr in one respect: as well as
 * checking that every file arrived with the right contents, it reads each
 * directory back with iterate_dir() and requires the entries to be
 * exactly the expected set, so a stray or missing name is caught rather
 * than only a differing file.
 *
 * Deviations: 2 dirs x 3 files x 3 deep at 4k, not 3 x 6 x 5 at 10k --
 * 45 files rather than about 1500, because every one of them is a
 * round-trip to the server and the export is a 64 MiB tmpfs.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>

#include "xfstests_nfs_fixture.h"

#define G100_SRC	"/g100src"		/* ramfs: upstream's /tmp */
#define G100_ROOT	XFS_MNT "/g100"
#define G100_DST	G100_ROOT "/populate_root"

#define G100_DIRS	2
#define G100_FILES	3
#define G100_DEPTH	3
#define G100_FILESZ	4096

struct g100_dirents {
	struct dir_context	ctx;
	struct kunit		*test;
	const char		*path;
	int			dirs;
	int			files;
	int			alien;
	int			total;		/* including . and .. */
};

static bool g100_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g100_dirents *it = container_of(ctx, struct g100_dirents, ctx);

	it->total++;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (len == 2 && name[0] == 'd' && name[1] >= '0' &&
	    name[1] < '0' + G100_DIRS)
		it->dirs++;
	else if (len == 2 && name[0] == 'f' && name[1] >= '0' &&
		 name[1] < '0' + G100_FILES)
		it->files++;
	else
		it->alien++;
	return true;
}

/* every byte of every file identifies the file it belongs to */
static u8 g100_byte(unsigned int seed, int off)
{
	return (u8)(seed * 31 + off);
}

static void g100_fill(u8 *buf, unsigned int seed)
{
	int i;

	for (i = 0; i < G100_FILESZ; i++)
		buf[i] = g100_byte(seed, i);
}

/* build one level of the source tree in the local filesystem */
static void g100_build(struct kunit *test, char *path, int depth,
		       unsigned int *seed, u8 *buf)
{
	size_t len = strlen(path);
	int i;

	for (i = 0; i < G100_FILES; i++) {
		snprintf(path + len, 8, "/f%d", i);
		g100_fill(buf, ++(*seed));
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_write_new_file(path, buf, G100_FILESZ),
				    0, "creating %s failed", path);
		path[len] = '\0';
	}
	if (depth == 0)
		return;
	for (i = 0; i < G100_DIRS; i++) {
		snprintf(path + len, 8, "/d%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(path), 0,
				    "creating %s failed", path);
		g100_build(test, path, depth - 1, seed, buf);
		path[len] = '\0';
	}
}

/* copy src/ into dst/, which must already exist */
static void g100_copy(struct kunit *test, char *src, char *dst, int depth,
		      u8 *buf)
{
	size_t slen = strlen(src), dlen = strlen(dst);
	int i;

	for (i = 0; i < G100_FILES; i++) {
		snprintf(src + slen, 8, "/f%d", i);
		snprintf(dst + dlen, 8, "/f%d", i);
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(src, buf, G100_FILESZ, 0),
				(ssize_t)G100_FILESZ);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_write_new_file(dst, buf, G100_FILESZ),
				    0, "copying to %s failed", dst);
		src[slen] = '\0';
		dst[dlen] = '\0';
	}
	if (depth == 0)
		return;
	for (i = 0; i < G100_DIRS; i++) {
		snprintf(src + slen, 8, "/d%d", i);
		snprintf(dst + dlen, 8, "/d%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(dst), 0,
				    "creating %s failed", dst);
		g100_copy(test, src, dst, depth - 1, buf);
		src[slen] = '\0';
		dst[dlen] = '\0';
	}
}

static void g100_verify_dir(struct kunit *test, const char *path, int depth)
{
	struct g100_dirents it = {
		.ctx.actor = g100_actor,
		.test = test,
		.path = path,
	};
	struct file *d;
	int before;

	d = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "%s: open: %ld", path,
			       PTR_ERR(d));
	/* one iterate_dir() is one getdents(2): a batch, not the whole dir */
	do {
		before = it.total;
		KUNIT_ASSERT_EQ(test, iterate_dir(d, &it.ctx), 0);
	} while (it.total > before);
	filp_close(d, NULL);

	KUNIT_EXPECT_EQ_MSG(test, it.files, G100_FILES,
			    "%s holds %d files", path, it.files);
	KUNIT_EXPECT_EQ_MSG(test, it.dirs, depth ? G100_DIRS : 0,
			    "%s holds %d subdirectories", path, it.dirs);
	KUNIT_EXPECT_EQ_MSG(test, it.alien, 0,
			    "%s holds %d unexpected entries", path, it.alien);
}

static void g100_verify(struct kunit *test, char *path, int depth,
			unsigned int *seed, u8 *buf)
{
	size_t len = strlen(path);
	int i, j;

	g100_verify_dir(test, path, depth);

	for (i = 0; i < G100_FILES; i++) {
		unsigned int s = ++(*seed);

		snprintf(path + len, 8, "/f%d", i);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_read_range(path, buf, G100_FILESZ, 0),
				    (ssize_t)G100_FILESZ,
				    "%s: short read", path);
		for (j = 0; j < G100_FILESZ; j++)
			if (buf[j] != g100_byte(s, j)) {
				KUNIT_FAIL(test, "%s: byte %d is %02x", path,
					   j, buf[j]);
				break;
			}
		path[len] = '\0';
	}
	if (depth == 0)
		return;
	for (i = 0; i < G100_DIRS; i++) {
		snprintf(path + len, 8, "/d%d", i);
		g100_verify(test, path, depth - 1, seed, buf);
		path[len] = '\0';
	}
}

static void g100_rmtree(char *path, int depth)
{
	size_t len = strlen(path);
	int i;

	for (i = 0; i < G100_FILES; i++) {
		snprintf(path + len, 8, "/f%d", i);
		xfs_unlink(path);
		path[len] = '\0';
	}
	if (depth) {
		for (i = 0; i < G100_DIRS; i++) {
			snprintf(path + len, 8, "/d%d", i);
			g100_rmtree(path, depth - 1);
			path[len] = '\0';
		}
	}
	xfs_rmdir_settled(path);
}

static char g100_path[128];

static void g100_remove_tree(void *unused)
{
	strscpy(g100_path, G100_DST, sizeof(g100_path));
	g100_rmtree(g100_path, G100_DEPTH);
	xfs_rmdir_settled(G100_ROOT);
	strscpy(g100_path, G100_SRC, sizeof(g100_path));
	g100_rmtree(g100_path, G100_DEPTH);
}

static void a_copied_tree_matches_the_original(struct kunit *test)
{
	static char dst[128];
	unsigned int seed = 0;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G100_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g100_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G100_SRC), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G100_DST), 0);

	buf = kunit_kmalloc(test, G100_FILESZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	strscpy(g100_path, G100_SRC, sizeof(g100_path));
	g100_build(test, g100_path, G100_DEPTH, &seed, buf);

	strscpy(g100_path, G100_SRC, sizeof(g100_path));
	strscpy(dst, G100_DST, sizeof(dst));
	g100_copy(test, g100_path, dst, G100_DEPTH, buf);

	seed = 0;
	strscpy(g100_path, G100_DST, sizeof(g100_path));
	g100_verify(test, g100_path, G100_DEPTH, &seed, buf);
}

static int g100_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g100_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g100_cases[] = {
	KUNIT_CASE_SLOW(a_copied_tree_matches_the_original),
	{}
};

static struct kunit_suite g100_suite = {
	.name		= "xfstests/generic/100",
	.suite_init	= g100_suite_init,
	.suite_exit	= g100_suite_exit,
	.test_cases	= g100_cases,
};

kunit_test_suites(&g100_suite);

MODULE_DESCRIPTION("xfstests generic/100 over a loopback NFS mount");
MODULE_LICENSE("GPL");
