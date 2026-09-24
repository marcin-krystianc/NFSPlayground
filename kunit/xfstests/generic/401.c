// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/401 over a loopback NFS mount: d_type in readdir.
 *
 * Upstream creates one entry of each type -- directory, regular file,
 * symlink, character device, block device and fifo -- and walks the
 * directory with src/t_dir_type. The golden output wants every entry's
 * real type, and DT_DIR for "." and "..". Only when _supports_filetype
 * says no may a DT_UNKNOWN stand in. For NFS, _supports_filetype touches
 * a file in the mount root and passes if t_dir_type finds no DT_UNKNOWN
 * entry there.
 *
 * Over NFSv4 the type does not come from any on-disk directory entry: the
 * client asks for the type attribute in READDIR and turns it into a
 * d_type (nfs4_decode_dirent -> nfs_readdir_page_filler). The port
 * asserts the exact types, which is upstream's result when the probe
 * passes; it does not run the probe, so a client that reported
 * DT_UNKNOWN everywhere fails here where upstream would relax.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/dcache.h>

#include "xfstests_nfs_fixture.h"

#define G401_ROOT	XFS_MNT "/g401"
#define G401_DIR	G401_ROOT "/find-by-type"

static const struct g401_entry {
	const char	*name;
	unsigned int	type;
} g401_entries[] = {
	{ "d",	DT_DIR },
	{ "f",	DT_REG },
	{ "l",	DT_LNK },
	{ "c",	DT_CHR },
	{ "b",	DT_BLK },
	{ "p",	DT_FIFO },
};

struct g401_iter {
	struct dir_context	ctx;
	unsigned int		seen[ARRAY_SIZE(g401_entries)];
	int			alien;
	unsigned int		dots;
	unsigned int		dot_type[2];	/* ".", ".." */
	int			total;		/* including . and .. */
};

static bool g401_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g401_iter *it = container_of(ctx, struct g401_iter, ctx);
	int i;

	it->total++;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.')) {
		it->dot_type[len - 1] = type;
		it->dots++;
		return true;
	}
	for (i = 0; i < ARRAY_SIZE(g401_entries); i++)
		if (len == 1 && name[0] == g401_entries[i].name[0]) {
			it->seen[i] = type + 1;	/* 0 means "not seen" */
			return true;
		}
	it->alien++;
	return true;
}

static void g401_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g401_entries); i++) {
		snprintf(path, sizeof(path), G401_DIR "/%s",
			 g401_entries[i].name);
		if (g401_entries[i].type == DT_DIR)
			xfs_rmdir(path);
		else
			xfs_unlink(path);
	}
	xfs_rmdir_settled(G401_DIR);
	xfs_rmdir_settled(G401_ROOT);
}

static void readdir_reports_every_entry_type(struct kunit *test)
{
	struct g401_iter it = { .ctx.actor = g401_actor };
	struct file *d;
	int i, before;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G401_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g401_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G401_DIR), 0);

	KUNIT_ASSERT_EQ(test, xfs_mkdir(G401_DIR "/d"), 0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G401_DIR "/f", "", 0), 0);
	KUNIT_ASSERT_EQ(test, xfs_symlink(G401_DIR "/f", G401_DIR "/l"), 0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G401_DIR "/c", S_IFCHR | 0644, 1, 1),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G401_DIR "/b", S_IFBLK | 0644, 1, 1),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mknod(G401_DIR "/p", S_IFIFO | 0644, 0, 0),
			0);

	d = filp_open(G401_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));
	/* one iterate_dir() is one getdents(2): a batch, not the whole dir */
	do {
		before = it.total;
		KUNIT_ASSERT_EQ(test, iterate_dir(d, &it.ctx), 0);
	} while (it.total > before);
	filp_close(d, NULL);

	KUNIT_EXPECT_EQ_MSG(test, it.alien, 0, "%d unexpected entries",
			    it.alien);
	KUNIT_EXPECT_EQ_MSG(test, it.dots, 2U, "%u dot entries", it.dots);
	KUNIT_EXPECT_EQ_MSG(test, it.dot_type[0], (unsigned int)DT_DIR,
			    ". came back with d_type %u", it.dot_type[0]);
	KUNIT_EXPECT_EQ_MSG(test, it.dot_type[1], (unsigned int)DT_DIR,
			    ".. came back with d_type %u", it.dot_type[1]);

	for (i = 0; i < ARRAY_SIZE(g401_entries); i++) {
		const struct g401_entry *e = &g401_entries[i];

		KUNIT_EXPECT_NE_MSG(test, it.seen[i], 0U,
				    "entry %s never came back from readdir",
				    e->name);
		if (it.seen[i])
			KUNIT_EXPECT_EQ_MSG(test, it.seen[i] - 1, e->type,
					    "entry %s came back with d_type %u, expected %u",
					    e->name, it.seen[i] - 1, e->type);
	}
}

static int g401_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g401_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g401_cases[] = {
	KUNIT_CASE(readdir_reports_every_entry_type),
	{}
};

static struct kunit_suite g401_suite = {
	.name		= "xfstests/generic/401",
	.suite_init	= g401_suite_init,
	.suite_exit	= g401_suite_exit,
	.test_cases	= g401_cases,
};

kunit_test_suites(&g401_suite);

MODULE_DESCRIPTION("xfstests generic/401 over a loopback NFS mount");
MODULE_LICENSE("GPL");
