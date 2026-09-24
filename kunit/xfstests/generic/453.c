// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/453 over a loopback NFS mount: filenames are byte
 * strings, not text.
 *
 * Upstream fills a directory with names that render the same, or nearly,
 * but differ byte for byte -- NFC against NFD accents, full- against
 * half-width, an Arabic ligature against its expansion, an overlong
 * slash, box drawing with embedded newlines, right-to-left overrides,
 * confusable job-offer.pdf lookalikes, zero-width joiners, variation
 * selectors, hidden tag characters, emoji with skin tones -- and requires
 * each to keep its own contents:
 *
 *	setf key value		echo value > $testdir/key
 *	setd key value		mkdir $testdir/key; echo value > .../key/value
 *	setchild dir key	mkdir $testdir/dir; echo dir > .../dir/key
 *
 * then reads every one back with cat (so without the trailing newline),
 * and requires the non-hidden top-level entries to be distinct inodes.
 * The xfs_scrub step only runs on XFS. The tables below are upstream's
 * lists, decoded from their echo -e escapes; one name keeps the literal
 * newline upstream's script has inside its quotes. Several entries share
 * a value on purpose, so only the name tells them apart.
 *
 * Over NFS a name is an opaque component4 (RFC 7530 only says it
 * "should" be UTF-8), so this checks that each byte sequence survives
 * CREATE, LOOKUP, READDIR and READ unchanged rather than being rejected,
 * re-encoded or confused with its neighbour. The overlong slash (c0 af)
 * is the pointed case: it must stay one component.
 *
 * Beyond upstream, the directory is also read back with getdents, and
 * every top-level name must appear in it exactly once.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G453_ROOT	XFS_MNT "/g453"
#define G453_DIR	G453_ROOT "/test-453"
#define G453_PATH	512

struct g453_entry {
	const char	*name;
	const char	*value;
};

static const struct g453_entry g453_files[] = {
	{ "french_caf\303\251.txt",
	  "NFC" },
	{ "french_cafe\314\201.txt",
	  "NFD" },
	{ "chinese_\357\275\266.txt",
	  "NFKC1" },
	{ "chinese_\343\202\253.txt",
	  "NFKC2" },
	{ "greek_\317\223.txt",
	  "GREEK UPSILON WITH ACUTE AND HOOK SYMBOL, NFC" },
	{ "greek_\317\222\314\201.txt",
	  "GREEK UPSILON WITH ACUTE AND HOOK SYMBOL, NFD" },
	{ "greek_\316\216.txt",
	  "GREEK UPSILON WITH ACUTE AND HOOK SYMBOL, NFKC" },
	{ "greek_\316\245\314\201.txt",
	  "GREEK UPSILON WITH ACUTE AND HOOK SYMBOL, NFKD" },
	{ "arabic_\357\267\272.txt",
	  "ARABIC LIGATURE SALLALLAHOU ALAYHE WASALLAM, NFC" },
	{ "arabic_\330\265\331\204\331\211 \330\247\331\204\331\204\331\207 \330\271\331\204\331\212\331\207 \331\210\330\263\331\204\331\205.txt",
	  "ARABIC LIGATURE SALLALLAHOU ALAYHE WASALLAM, NFKC" },
	{ "urk\300\257moo",
	  "FAKESLASH" },
	{ "emoji_\360\237\246\221\360\237\246\213\360\237\246\211\360\237\246\222.txt",
	  "octopus butterfly owl giraffe emoji" },
	{ "linedraw_\012\342\225\224\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\227\012\342\225\221 metatable \342\225\221\012\342\225\237\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\225\242\012\342\225\221 __index   \342\225\221\012\342\225\232\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\220\342\225\235\012.txt",
	  "ugly box because we can" },
	{ "moo\342\200\256gnp.txt",
	  "Well say hello," },
	{ "mootxt.png",
	  "Harvey" },
	{ "mixed_t\316\277p.txt",
	  "greek omicron instead of o" },
	{ "mixed_top.txt",
	  "greek omicron instead of o" },
	{ "hyphens_a\342\200\220b.txt",
	  "hyphens" },
	{ "hyphens_a-b.txt",
	  "hyphens" },
	{ "dz_digraph_dze.txt",
	  "d-z digraph" },
	{ "dz_digraph_\312\243e.txt",
	  "d-z digraph" },
	{ "inadequate_al.txt",
	  "is it l or is it 1" },
	{ "inadequate_a1.txt",
	  "is it l or is it 1" },
	{ "prohibition_Rs.txt",
	  "rupee symbol" },
	{ "prohibition_\342\202\250.txt",
	  "rupee symbol" },
	{ "zerojoin_moocow.txt",
	  "zero width joiners" },
	{ "zerojoin_moo\342\200\215cow.txt",
	  "zero width joiners" },
	{ "combmark_\341\200\234\341\200\255\341\200\257.txt",
	  "combining marks" },
	{ "combmark_\341\200\234\341\200\257\341\200\255.txt",
	  "combining marks" },
	{ "toilet_bowl.\360\237\232\275",
	  "toilet emoji" },
	{ "toilet_bow\342\200\215l.\360\237\232\275",
	  "toilet emoji with zero width joiner" },
	{ "job offer\342\200\244pdf",
	  "one dot leader" },
	{ "job offer\357\271\222pdf",
	  "small full stop" },
	{ "job offer\357\274\216pdf",
	  "fullwidth full stop" },
	{ "job offer\334\201pdf",
	  "syriac supralinear full stop" },
	{ "job offer\334\202pdf",
	  "syriac sublinear full stop" },
	{ "job offer\352\223\270pdf",
	  "li_su letter tone mya ti" },
	{ "job offer.pdf",
	  "actual period" },
	{ "llamapirate\363\240\200\201\363\240\201\224\363\240\201\250\363\240\201\245\363\240\200\240\363\240\201\263\363\240\201\241\363\240\201\254\363\240\201\245\363\240\201\263\363\240\200\240\363\240\201\246\363\240\201\257\363\240\201\262\363\240\200\240\363\240\201\223\363\240\201\245\363\240\201\241\363\240\201\264\363\240\201\264\363\240\201\254\363\240\201\245\363\240\200\240\363\240\201\267\363\240\201\245\363\240\201\262\363\240\201\245\363\240\200\240\363\240\201\225\363\240\201\223\363\240\201\204\363\240\200\240\363\240\200\261\363\240\200\262\363\240\200\260\363\240\200\260\363\240\200\260\363\240\200\260\363\240\201\277",
	  "" },
	{ "llamapirate",
	  "" },
	{ "\360\237\222\234",
	  "purple" },
	{ "\360\237\222\231",
	  "blue" },
	{ "\360\237\222\232",
	  "green" },
	{ "\360\237\222\233",
	  "yellow" },
	{ "\360\237\253\200",
	  "heart" },
	{ "\342\235\244\357\270\217",
	  "red" },
	{ "\360\237\244\216",
	  "brown" },
	{ "\360\237\244\215",
	  "white" },
	{ "\360\237\226\244",
	  "black" },
	{ "\360\237\247\241",
	  "orange" },
	{ "\342\231\245\357\270\217",
	  "red suit" },
	{ "\360\237\222\224",
	  "broken heart" },
	{ "\342\235\244\357\270\217\342\200\215\360\237\251\271",
	  "mending heart" },
	{ "\360\237\247\221\360\237\217\273\342\200\215\342\235\244\357\270\012\217\342\200\215\360\237\247\221\360\237\217\274",
	  "couple with heart" },
	{ "\360\237\247\221\360\237\217\273\342\200\215\342\235\244\357\270\217\342\200\215\360\237\247\221\360\237\217\277",
	  "couple with heart, light and dark skin tone" },
	{ "\360\237\253\266\360\237\217\277",
	  "dark" },
	{ "\360\237\253\266\360\237\217\276",
	  "medium dark" },
	{ "\360\237\253\266\360\237\217\275",
	  "medium" },
	{ "\360\237\253\266\360\237\217\274",
	  "medium light" },
	{ "\360\237\253\266\360\237\217\273",
	  "light" },
	{ "\360\237\253\266",
	  "neutral" },
	{ "variations.txt",
	  "v0" },
	{ "varia\357\270\200tions.txt",
	  "v1" },
	{ "\357\270\200variations.txt",
	  "v2" },
	{ "vari\357\270\200\357\270\201ations.txt",
	  "v3" },
	{ "varia\363\240\207\244tions.txt",
	  "v4" },
	{ "tags_moocow.txt",
	  "u0" },
	{ "tags_m\363\240\201\255oocow.txt",
	  "u1" },
	{ "\363\240\200\250\363\240\201\210\363\240\201\251\363\240\200\251",
	  "(Hi)" },
	{ "\327\242\327\221\327\250\327\231\327\252.pdf",
	  "mixed rtl and ltr chars\077" },
};

static const struct g453_entry g453_dirs[] = {
	{ ".\342\200\215",
	  "zero width joiners in dot entry" },
	{ "..\342\200\215",
	  "zero width joiners in dotdot entry" },
};

/* setchild dir key: name is the key, value the directory */
static const struct g453_entry g453_children[] = {
	{ "job offer\342\200\244pdf",
	  "one_dot_leader" },
	{ "job offer\357\271\222pdf",
	  "small_full_stop" },
	{ "job offer\357\274\216pdf",
	  "fullwidth_full_stop" },
	{ "job offer\334\201pdf",
	  "syriac_supralinear" },
	{ "job offer\334\202pdf",
	  "syriac_sublinear" },
	{ "job offer\352\223\270pdf",
	  "lisu_letter_tone" },
	{ "job offer.pdf",
	  "actual_period" },
	{ "job offer\342\200\244\342\200\215pdf",
	  "one_dot_leader_zero_width_space" },
};

#define G453_TOP	(ARRAY_SIZE(g453_files) + ARRAY_SIZE(g453_dirs) + \
			 ARRAY_SIZE(g453_children))

static void g453_path(char *buf, const char *a, const char *b)
{
	if (b)
		snprintf(buf, G453_PATH, G453_DIR "/%s/%s", a, b);
	else
		snprintf(buf, G453_PATH, G453_DIR "/%s", a);
}

/* echo "value" > path */
static int g453_echo(char *buf, const char *path, const char *value)
{
	size_t len = strlen(value);

	memcpy(buf, value, len);
	buf[len] = '\n';
	return xfs_write_new_file(path, buf, len + 1);
}

/* the file exists and cat, less its trailing newline, prints value */
static void g453_expect(struct kunit *test, char *buf, const char *path,
			const char *value, int i, const char *what)
{
	size_t len = strlen(value);
	ssize_t n;

	n = xfs_read_range(path, buf, G453_PATH, 0);
	KUNIT_ASSERT_GE_MSG(test, n, 0L, "%s %d does not exist? (%zd)", what,
			    i, n);
	if (n > 0 && buf[n - 1] == '\n')
		n--;
	KUNIT_EXPECT_TRUE_MSG(test, n == len && !memcmp(buf, value, len),
			      "%s %d has value %.*s, expected %s", what, i,
			      (int)n, buf, value);
}

static void g453_remove_tree(void *unused)
{
	char *path = kmalloc(G453_PATH, GFP_KERNEL);
	int i;

	if (!path)
		return;
	xfs_settle_fput();
	for (i = 0; i < ARRAY_SIZE(g453_files); i++) {
		g453_path(path, g453_files[i].name, NULL);
		xfs_unlink(path);
	}
	for (i = 0; i < ARRAY_SIZE(g453_dirs); i++) {
		g453_path(path, g453_dirs[i].name, "value");
		xfs_unlink(path);
		g453_path(path, g453_dirs[i].name, NULL);
		xfs_rmdir_settled(path);
	}
	for (i = 0; i < ARRAY_SIZE(g453_children); i++) {
		g453_path(path, g453_children[i].value, g453_children[i].name);
		xfs_unlink(path);
		g453_path(path, g453_children[i].value, NULL);
		xfs_rmdir_settled(path);
	}
	xfs_rmdir_settled(G453_DIR);
	xfs_rmdir_settled(G453_ROOT);
	kfree(path);
}

/* the top-level name at index i: files, then dirs, then children's dirs */
static const char *g453_top(int i)
{
	if (i < ARRAY_SIZE(g453_files))
		return g453_files[i].name;
	i -= ARRAY_SIZE(g453_files);
	if (i < ARRAY_SIZE(g453_dirs))
		return g453_dirs[i].name;
	return g453_children[i - ARRAY_SIZE(g453_dirs)].value;
}

static void names_that_look_alike_stay_different(struct kunit *test)
{
	struct xfs_dirent *ents;
	char *path, *buf;
	u64 *inos;
	u8 *seen;
	struct kstat st;
	struct file *d;
	int i, j, n, alien = 0, nino = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G453_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g453_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G453_DIR), 0);
	path = kunit_kmalloc(test, G453_PATH, GFP_KERNEL);
	buf = kunit_kmalloc(test, G453_PATH, GFP_KERNEL);
	inos = kunit_kcalloc(test, G453_TOP, sizeof(*inos), GFP_KERNEL);
	seen = kunit_kzalloc(test, G453_TOP, GFP_KERNEL);
	ents = kunit_kcalloc(test, 64, sizeof(*ents), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, inos);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, seen);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);

	/* Create files */
	for (i = 0; i < ARRAY_SIZE(g453_files); i++) {
		g453_path(path, g453_files[i].name, NULL);
		KUNIT_ASSERT_EQ_MSG(test,
				    g453_echo(buf, path, g453_files[i].value),
				    0, "storing file %d", i);
	}
	for (i = 0; i < ARRAY_SIZE(g453_dirs); i++) {
		g453_path(path, g453_dirs[i].name, NULL);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		g453_path(path, g453_dirs[i].name, "value");
		KUNIT_ASSERT_EQ_MSG(test,
				    g453_echo(buf, path, g453_dirs[i].value),
				    0, "storing dir %d", i);
	}
	for (i = 0; i < ARRAY_SIZE(g453_children); i++) {
		g453_path(path, g453_children[i].value, NULL);
		KUNIT_ASSERT_EQ(test, xfs_mkdir(path), 0);
		g453_path(path, g453_children[i].value, g453_children[i].name);
		KUNIT_ASSERT_EQ_MSG(test,
				    g453_echo(buf, path, g453_children[i].value),
				    0, "storing child %d", i);
	}

	/* Test files */
	for (i = 0; i < ARRAY_SIZE(g453_files); i++) {
		g453_path(path, g453_files[i].name, NULL);
		g453_expect(test, buf, path, g453_files[i].value, i, "file");
	}
	for (i = 0; i < ARRAY_SIZE(g453_dirs); i++) {
		g453_path(path, g453_dirs[i].name, "value");
		g453_expect(test, buf, path, g453_dirs[i].value, i, "dir");
	}
	for (i = 0; i < ARRAY_SIZE(g453_children); i++) {
		g453_path(path, g453_children[i].value, g453_children[i].name);
		g453_expect(test, buf, path, g453_children[i].value, i,
			    "child");
	}

	/* Uniqueness of inodes? stat of $testdir/*, which skips dot names */
	for (i = 0; i < G453_TOP; i++) {
		if (g453_top(i)[0] == '.')
			continue;
		g453_path(path, g453_top(i), NULL);
		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		for (j = 0; j < nino; j++)
			KUNIT_EXPECT_NE_MSG(test, inos[j], st.ino,
					    "entry %d shares inode %llu", i,
					    st.ino);
		inos[nino++] = st.ino;
	}

	/* not upstream: getdents returns every top-level name once */
	d = filp_open(G453_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));
	while ((n = xfs_getdents(d, ents, 64, 32768)) > 0) {
		for (j = 0; j < n; j++) {
			if (!strcmp(ents[j].name, ".") ||
			    !strcmp(ents[j].name, ".."))
				continue;
			for (i = 0; i < G453_TOP; i++)
				if (!strcmp(ents[j].name, g453_top(i)))
					break;
			if (i < G453_TOP)
				seen[i]++;
			else
				alien++;
		}
	}
	filp_close(d, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	for (i = 0; i < G453_TOP; i++)
		KUNIT_EXPECT_EQ_MSG(test, seen[i], 1,
				    "getdents returned top-level entry %d %u times",
				    i, seen[i]);
	KUNIT_EXPECT_EQ_MSG(test, alien, 0,
			    "getdents returned %d unexpected names", alien);
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
	KUNIT_CASE(names_that_look_alike_stay_different),
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
