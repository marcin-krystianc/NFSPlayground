// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/109 over a loopback NFS mount: rename and the entry type.
 *
 * Upstream's motivation, from its header, is "a bug in XFS where directory
 * entry file type was not updated properly on rename". Its renamedir() is
 * therefore not a set of plain renames: it renames regular files and
 * symlinks over each other and over names that do not exist, six shapes in
 * all, so that every combination of old and new entry type occurs --
 *
 *	mv -T fs1 fd1	file over file
 *	mv -T fs2 sd1	file over symlink
 *	mv -T fs3 ed1	file over nothing
 *	mv -T ss1 fd2	symlink over file
 *	mv -T ss2 sd2	symlink over symlink
 *	mv -T ss3 ed2	symlink over nothing
 *
 * and it repeats the whole thing for twenty directory sizes, to cross the
 * points where a filesystem changes its directory representation.
 *
 * Over NFS the equivalent surface is the client's dcache and the server's
 * directory: the type of a name that was just renamed over has to be the
 * source's, both when stat'ed and when read back out of the directory.
 * Upstream detects a wrong type through its post-test filesystem check;
 * there is no fsck for a live NFS mount, so the type is asserted directly --
 * authoritatively via GETATTR, and additionally against READDIR's d_type
 * where the server supplies one (a plain READDIR without readdirplus
 * legitimately reports DT_UNKNOWN).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>

#include "xfstests_nfs_fixture.h"

#define G109_ROOT	XFS_MNT "/g109"
#define G109_DIR	G109_ROOT "/dir"

/* upstream's twenty sizes, verbatim */
static const int g109_sizes[] = {
	1, 2, 3, 4, 5, 8, 12, 18, 27, 40,
	60, 90, 135, 202, 303, 454, 681, 1020, 1530, 2295,
};

/* the names renamedir() creates and the type each must end up with */
static const struct g109_target {
	const char	*name;
	bool		is_link;	/* the source's type */
} g109_targets[] = {
	{ "fd1", false },	/* fs1 (file)    -> fd1 (was a file)    */
	{ "sd1", false },	/* fs2 (file)    -> sd1 (was a symlink) */
	{ "ed1", false },	/* fs3 (file)    -> ed1 (did not exist) */
	{ "fd2", true  },	/* ss1 (symlink) -> fd2 (was a file)    */
	{ "sd2", true  },	/* ss2 (symlink) -> sd2 (was a symlink) */
	{ "ed2", true  },	/* ss3 (symlink) -> ed2 (did not exist) */
};

static const char * const g109_sources[] = {
	"fs1", "fs2", "fs3", "ss1", "ss2", "ss3",
};

/* READDIR's view: the d_type reported for one name, or DT_UNKNOWN */
struct g109_iter {
	struct dir_context	ctx;
	const char		*want;
	unsigned int		type;
	bool			found;
};

static bool g109_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g109_iter *it = container_of(ctx, struct g109_iter, ctx);

	if (len == strlen(it->want) && !memcmp(name, it->want, len)) {
		it->type = type;
		it->found = true;
	}
	return true;
}

static int g109_readdir_type(const char *dir, const char *name,
			     unsigned int *type, bool *found)
{
	struct g109_iter it = { .ctx.actor = g109_actor, .want = name,
				.type = DT_UNKNOWN };
	struct file *d;
	int err;

	d = filp_open(dir, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(d))
		return PTR_ERR(d);
	err = iterate_dir(d, &it.ctx);
	filp_close(d, NULL);
	*type = it.type;
	*found = it.found;
	return err;
}

static void g109_path(char *buf, size_t size, const char *name)
{
	snprintf(buf, size, G109_DIR "/%s", name);
}

/*
 * How many fname* entries the directory currently holds, so the teardown
 * action does not have to sweep the whole ladder's worth of names (each miss
 * is a LOOKUP round trip).
 */
static int g109_cur_files;

static void g109_empty_dir(const char *dir, int nfiles)
{
	char buf[96];
	int i;

	for (i = 0; i < nfiles; i++) {
		snprintf(buf, sizeof(buf), "%s/fname%d", dir, i);
		xfs_unlink(buf);
	}
	for (i = 0; i < ARRAY_SIZE(g109_sources); i++) {
		snprintf(buf, sizeof(buf), "%s/%s", dir, g109_sources[i]);
		xfs_unlink(buf);
	}
	for (i = 0; i < ARRAY_SIZE(g109_targets); i++) {
		snprintf(buf, sizeof(buf), "%s/%s", dir, g109_targets[i].name);
		xfs_unlink(buf);
	}
	xfs_rmdir_settled(dir);
}

static void g109_remove_dir(int nfiles)
{
	g109_empty_dir(G109_DIR, nfiles);
	g109_cur_files = 0;
}

static void g109_remove_tree(void *unused)
{
	/* whichever of the two names the run may have left behind */
	g109_empty_dir(G109_DIR, g109_cur_files);
	g109_empty_dir(G109_ROOT "/moved", g109_cur_files);
	g109_cur_files = 0;
	xfs_rmdir(G109_ROOT);
}

/* filldir(): $1 empty regular files */
static void g109_filldir(struct kunit *test, int nfiles)
{
	char buf[96];
	int i;

	for (i = 0; i < nfiles; i++) {
		snprintf(buf, sizeof(buf), G109_DIR "/fname%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(buf, "", 0), 0,
				    "creating fname%d failed", i);
		g109_cur_files = i + 1;
	}
}

/* renamedir(): the six rename shapes, in upstream's order */
static void g109_renamedir(struct kunit *test, int nfiles)
{
	static const char * const files[] = { "fs1", "fs2", "fs3", "fd1", "fd2" };
	static const char * const links[] = { "ss1", "ss2", "ss3", "sd1", "sd2" };
	static const struct { const char *from, *to; } renames[] = {
		{ "fs1", "fd1" }, { "fs2", "sd1" }, { "fs3", "ed1" },
		{ "ss1", "fd2" }, { "ss2", "sd2" }, { "ss3", "ed2" },
	};
	char a[96], b[96];
	int i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		g109_path(a, sizeof(a), files[i]);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(a, "", 0), 0,
				    "size %d: touch %s", nfiles, files[i]);
	}
	for (i = 0; i < ARRAY_SIZE(links); i++) {
		g109_path(a, sizeof(a), links[i]);
		KUNIT_ASSERT_EQ_MSG(test, xfs_symlink("foo", a), 0,
				    "size %d: symlink %s", nfiles, links[i]);
	}

	for (i = 0; i < ARRAY_SIZE(renames); i++) {
		g109_path(a, sizeof(a), renames[i].from);
		g109_path(b, sizeof(b), renames[i].to);
		KUNIT_ASSERT_EQ_MSG(test, xfs_rename(a, b), 0,
				    "size %d: rename %s -> %s", nfiles,
				    renames[i].from, renames[i].to);
	}
}

/* every renamed name carries the source's type, and no source name remains */
static void g109_check_types(struct kunit *test, int nfiles)
{
	char path[96];
	int i;

	for (i = 0; i < ARRAY_SIZE(g109_sources); i++) {
		g109_path(path, sizeof(path), g109_sources[i]);
		KUNIT_EXPECT_FALSE_MSG(test, xfs_exists(path),
				       "size %d: the rename source %s survives",
				       nfiles, g109_sources[i]);
	}

	for (i = 0; i < ARRAY_SIZE(g109_targets); i++) {
		const struct g109_target *t = &g109_targets[i];
		unsigned int dtype;
		struct kstat st;
		bool found;

		g109_path(path, sizeof(path), t->name);

		/* AT_SYMLINK_NOFOLLOW is not needed: the symlinks point at
		 * "foo", which does not exist, so a stat that followed them
		 * would fail rather than mislead. xfs_kstat() does not follow.
		 */
		KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0,
				    "size %d: %s is missing after the rename",
				    nfiles, t->name);
		if (t->is_link)
			KUNIT_EXPECT_TRUE_MSG(test, S_ISLNK(st.mode),
					      "size %d: %s should be a symlink, mode is %o",
					      nfiles, t->name, (unsigned int)st.mode);
		else
			KUNIT_EXPECT_TRUE_MSG(test, S_ISREG(st.mode),
					      "size %d: %s should be a regular file, mode is %o",
					      nfiles, t->name, (unsigned int)st.mode);

		KUNIT_ASSERT_EQ(test,
				g109_readdir_type(G109_DIR, t->name, &dtype,
						  &found), 0);
		KUNIT_EXPECT_TRUE_MSG(test, found,
				      "size %d: %s is not in the directory listing",
				      nfiles, t->name);
		if (dtype != DT_UNKNOWN)
			KUNIT_EXPECT_EQ_MSG(test, dtype,
					    t->is_link ? DT_LNK : DT_REG,
					    "size %d: READDIR reports d_type %u for %s",
					    nfiles, dtype, t->name);
	}
}

static void renames_keep_the_entry_type_at_every_directory_size(struct kunit *test)
{
	int s;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G109_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g109_remove_tree, NULL),
			0);

	for (s = 0; s < ARRAY_SIZE(g109_sizes); s++) {
		int nfiles = g109_sizes[s];

		KUNIT_ASSERT_EQ_MSG(test, xfs_mkdir(G109_DIR), 0,
				    "size %d: mkdir", nfiles);
		g109_filldir(test, nfiles);
		g109_renamedir(test, nfiles);
		g109_check_types(test, nfiles);
		g109_remove_dir(nfiles);
	}
}

/*
 * The same six shapes with the target directory renamed underneath them
 * afterwards: the entries have to stay reachable, and keep their types,
 * through a path whose ancestor changed.
 */
static void renamed_directories_keep_their_entry_types(struct kunit *test)
{
	char path[96];
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G109_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g109_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_mkdir(G109_DIR), 0);
	g109_filldir(test, 8);
	g109_renamedir(test, 8);

	KUNIT_ASSERT_EQ(test, xfs_rename(G109_DIR, G109_ROOT "/moved"), 0);

	for (i = 0; i < ARRAY_SIZE(g109_targets); i++) {
		const struct g109_target *t = &g109_targets[i];
		struct kstat st;

		snprintf(path, sizeof(path), G109_ROOT "/moved/%s", t->name);
		KUNIT_ASSERT_EQ_MSG(test, xfs_kstat(path, &st), 0,
				    "%s unreachable after the parent was renamed",
				    t->name);
		KUNIT_EXPECT_EQ_MSG(test, S_ISLNK(st.mode), t->is_link,
				    "%s changed type when its parent was renamed",
				    t->name);
	}

	KUNIT_ASSERT_EQ(test, xfs_rename(G109_ROOT "/moved", G109_DIR), 0);
	g109_remove_dir(8);
}

static int g109_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g109_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g109_cases[] = {
	KUNIT_CASE_SLOW(renames_keep_the_entry_type_at_every_directory_size),
	KUNIT_CASE(renamed_directories_keep_their_entry_types),
	{}
};

static struct kunit_suite g109_suite = {
	.name		= "xfstests/generic/109",
	.suite_init	= g109_suite_init,
	.suite_exit	= g109_suite_exit,
	.test_cases	= g109_cases,
};

kunit_test_suites(&g109_suite);

MODULE_DESCRIPTION("xfstests generic/109 over a loopback NFS mount");
MODULE_LICENSE("GPL");
