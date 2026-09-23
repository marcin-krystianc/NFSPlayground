// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/453 over a loopback NFS mount: filenames are byte
 * strings, not text.
 *
 * Upstream creates a directory full of names that render the same (or
 * nearly the same) but differ byte for byte -- NFC vs NFD accents, an
 * Arabic ligature against its expansion, a right-to-left override, an
 * overlong-encoded slash, a zero-width joiner -- writes a different value
 * into each, and then requires every name to read back its own value and
 * every file to be a distinct inode. A filesystem that normalises or
 * folds names collapses two of them and fails.
 *
 * This is worth more over NFS than over a local filesystem. RFC 7530
 * says component names "should" be UTF-8 and lets a server reject what is
 * not (NFS4ERR_INVAL), and the client encodes names as opaque bytes; so
 * this checks that an unusual byte sequence survives LOOKUP, CREATE,
 * READDIR and READ unchanged rather than being rejected, re-encoded, or
 * confused with its neighbour. The overlong-encoded slash (c0 af) is the
 * pointed case: it must stay one component, not become a path separator.
 *
 * Deviations: a representative subset of upstream's names, since each one
 * is a round trip and they exercise the same property; the "uniqueness of
 * inodes" check is done on the inode numbers the server hands back rather
 * than by sorting `stat` output; and readdir is checked too, which
 * upstream only does via its scrub step.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G453_ROOT	XFS_MNT "/g453"

static const struct g453_name {
	const char	*name;
	const char	*value;
} g453_names[] = {
	{ "french_caf\xc3\xa9.txt",		"NFC" },
	{ "french_cafe\xcc\x81.txt",		"NFD" },
	{ "chinese_\xef\xbd\xb6.txt",		"NFKC1" },
	{ "chinese_\xe3\x82\xab.txt",		"NFKC2" },
	{ "greek_\xcf\x93.txt",			"GREEK NFC" },
	{ "greek_\xcf\x92\xcc\x81.txt",		"GREEK NFD" },
	{ "urk\xc0\xaf" "moo",			"FAKESLASH" },
	{ "emoji_\xf0\x9f\xa6\x91\xf0\x9f\xa6\x8b.txt", "octopus butterfly" },
	{ "moo\xe2\x80\xae" "gnp.txt",		"right-to-left override" },
	{ "mootxt.png",				"Harvey" },
	{ "hyphens_a\xe2\x80\x90" "b.txt",	"unicode hyphen" },
	{ "hyphens_a-b.txt",			"ascii hyphen" },
	{ "zerojoin_moo\xe2\x80\x8d" "cow.txt",	"with joiner" },
	{ "zerojoin_moocow.txt",		"without joiner" },
};

/* upstream's fake dotdot: a directory whose name is "." plus a joiner */
#define G453_DOTDIR	G453_ROOT "/." "\xe2\x80\x8d"
#define G453_DOTFILE	G453_DOTDIR "/value"
#define G453_DOTVALUE	"zero width joiners in dot entry"

struct g453_iter {
	struct dir_context	ctx;
	u8			seen[ARRAY_SIZE(g453_names)];
	int			dirs;
	int			alien;
	int			total;		/* including . and .. */
};

static bool g453_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g453_iter *it = container_of(ctx, struct g453_iter, ctx);
	int i;

	it->total++;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (type == DT_DIR) {
		it->dirs++;
		return true;
	}
	for (i = 0; i < ARRAY_SIZE(g453_names); i++)
		if (len == strlen(g453_names[i].name) &&
		    !memcmp(name, g453_names[i].name, len)) {
			it->seen[i]++;
			return true;
		}
	it->alien++;
	return true;
}

static void g453_remove_tree(void *unused)
{
	char path[256];
	int i;

	for (i = 0; i < ARRAY_SIZE(g453_names); i++) {
		snprintf(path, sizeof(path), G453_ROOT "/%s",
			 g453_names[i].name);
		xfs_unlink(path);
	}
	xfs_unlink(G453_DOTFILE);
	xfs_rmdir_settled(G453_DOTDIR);
	xfs_rmdir_settled(G453_ROOT);
}

static void names_that_look_alike_stay_different_files(struct kunit *test)
{
	struct g453_iter it = { .ctx.actor = g453_actor };
	static u64 inos[ARRAY_SIZE(g453_names)];
	char path[256], buf[64];
	struct kstat st;
	struct file *d;
	ssize_t n;
	int i, j, before;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G453_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g453_remove_tree, NULL),
			0);

	for (i = 0; i < ARRAY_SIZE(g453_names); i++) {
		const struct g453_name *e = &g453_names[i];

		snprintf(path, sizeof(path), G453_ROOT "/%s", e->name);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_write_new_file(path, e->value,
						       strlen(e->value)),
				    0, "creating entry %d failed", i);
	}

	/* every name reads back its own value */
	for (i = 0; i < ARRAY_SIZE(g453_names); i++) {
		const struct g453_name *e = &g453_names[i];
		size_t len = strlen(e->value);

		snprintf(path, sizeof(path), G453_ROOT "/%s", e->name);
		n = xfs_read_range(path, buf, sizeof(buf), 0);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
				    "entry %d read returned %zd", i, n);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(buf, e->value, len), 0,
				    "entry %d holds the wrong value", i);

		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		inos[i] = st.ino;
	}

	/* uniqueness of inodes */
	for (i = 0; i < ARRAY_SIZE(g453_names); i++)
		for (j = i + 1; j < ARRAY_SIZE(g453_names); j++)
			KUNIT_EXPECT_NE_MSG(test, inos[i], inos[j],
					    "entries %d and %d are the same inode (%llu)",
					    i, j, inos[i]);

	/* the fake dotdot directory, and a file inside it */
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G453_DOTDIR), 0);
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G453_DOTFILE, G453_DOTVALUE,
					   strlen(G453_DOTVALUE)), 0);
	n = xfs_read_range(G453_DOTFILE, buf, sizeof(buf), 0);
	KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)strlen(G453_DOTVALUE),
			    "the dot-joiner directory's file read returned %zd",
			    n);

	/* and readdir returns each name exactly once */
	d = filp_open(G453_ROOT, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));
	/* one iterate_dir() is one getdents(2): a batch, not the whole dir */
	do {
		before = it.total;
		KUNIT_ASSERT_EQ(test, iterate_dir(d, &it.ctx), 0);
	} while (it.total > before);
	filp_close(d, NULL);

	for (i = 0; i < ARRAY_SIZE(g453_names); i++)
		KUNIT_EXPECT_EQ_MSG(test, it.seen[i], 1,
				    "readdir returned entry %d %u times", i,
				    it.seen[i]);
	KUNIT_EXPECT_EQ_MSG(test, it.alien, 0,
			    "readdir returned %d unexpected names", it.alien);
	KUNIT_EXPECT_EQ_MSG(test, it.dirs, 1,
			    "readdir found %d subdirectories", it.dirs);
}

static int g453_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g453_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g453_cases[] = {
	KUNIT_CASE(names_that_look_alike_stay_different_files),
	{}
};

static struct kunit_suite g453_suite = {
	.name		= "xfstests/generic/453",
	.suite_init	= g453_suite_init,
	.suite_exit	= g453_suite_exit,
	.test_cases	= g453_cases,
};

kunit_test_suites(&g453_suite);

MODULE_DESCRIPTION("xfstests generic/453 over a loopback NFS mount");
MODULE_LICENSE("GPL");
