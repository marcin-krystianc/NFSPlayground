// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/454 over a loopback NFS mount: xattr names are byte
 * strings, not text.
 *
 * Upstream is generic/453's experiment moved from filenames to attribute
 * names: setfattr user.<key> for 61 keys that render alike, or nearly,
 * but differ byte for byte -- NFC against NFD, full- against half-width,
 * an Arabic ligature against its expansion, an overlong slash, box
 * drawing with embedded newlines, zero-width joiners, variation
 * selectors, hidden tag characters, emoji with skin tones -- then reads
 * each one back with getfattr and requires its own value. A filesystem
 * that normalises or folds names loses one of a pair. Several pairs share
 * a value on purpose, so only the key tells them apart.
 *
 * The table below is upstream's setf list, keys and values, decoded from
 * its echo -e escapes; one key keeps the literal newline upstream's script
 * has inside its quotes. xfs_setxattr() failing on any of them is what
 * upstream's "cat .output" would turn into a golden-image mismatch.
 *
 * Over NFSv4.2 each set is a SETXATTR and each read a GETXATTR (RFC
 * 8276), with the name carried as an opaque component -- so this checks
 * that unusual bytes survive the round trip and that two keys differing
 * only in encoding stay two keys. The values are read back through the
 * server's own copy of the file as well, because the client answers
 * GETXATTR from its xattr cache and a cache keyed on a folded name would
 * otherwise hide the bug; and the list must hold all 61 names.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "xfstests_nfs_fixture.h"

#define G454_ROOT	XFS_MNT "/g454"
#define G454_FILE	G454_ROOT "/attrfile"
#define G454_SERVER	XFS_EXPORT "/g454/attrfile"

static const struct g454_key {
	const char	*name;		/* without the user. prefix */
	const char	*value;
} g454_keys[] = {
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
	{ "zerojoin_moo\342\200\214cow.txt",
	  "zero width joiners" },
	{ "combmark_\341\200\234\341\200\255\341\200\257.txt",
	  "combining marks" },
	{ "combmark_\341\200\234\341\200\257\341\200\255.txt",
	  "combining marks" },
	{ "llamapirate\363\240\200\201\363\240\201\224\363\240\201\250\363\240\201\245\363\240\200\240\363\240\201\263\363\240\201\241\363\240\201\254\363\240\201\245\363\240\201\263\363\240\200\240\363\240\201\246\363\240\201\257\363\240\201\262\363\240\200\240\363\240\201\223\363\240\201\245\363\240\201\241\363\240\201\264\363\240\201\264\363\240\201\254\363\240\201\245\363\240\200\240\363\240\201\267\363\240\201\245\363\240\201\262\363\240\201\245\363\240\200\240\363\240\201\225\363\240\201\223\363\240\201\204\363\240\200\240\363\240\200\261\363\240\200\262\363\240\200\260\363\240\200\260\363\240\200\260\363\240\200\260\363\240\201\277",
	  "secret instructions" },
	{ "llamapirate",
	  "no secret instructions" },
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

static void g454_remove_tree(void *unused)
{
	xfs_unlink(G454_FILE);
	xfs_rmdir_settled(G454_ROOT);
}

static void g454_check_values(struct kunit *test, const char *path,
			      const char *which)
{
	char name[256], value[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g454_keys); i++) {
		const struct g454_key *k = &g454_keys[i];
		size_t len = strlen(k->value);
		ssize_t n;

		snprintf(name, sizeof(name), "user.%s", k->name);
		n = xfs_getxattr(path, name, value, sizeof(value));
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
				    "%s: getxattr of key %d returned %zd",
				    which, i, n);
		KUNIT_EXPECT_EQ_MSG(test, memcmp(value, k->value, len), 0,
				    "%s: key %d holds the wrong value", which,
				    i);
	}
}

static void keys_that_look_alike_stay_different_keys(struct kunit *test)
{
	char name[256], *list;
	ssize_t len, off;
	int i, count = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G454_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g454_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G454_FILE, "", 0), 0);

	for (i = 0; i < ARRAY_SIZE(g454_keys); i++) {
		const struct g454_key *k = &g454_keys[i];

		snprintf(name, sizeof(name), "user.%s", k->name);
		KUNIT_ASSERT_EQ_MSG(test,
				    xfs_setxattr(G454_FILE, name, k->value,
						 strlen(k->value), 0),
				    0, "setting key %d failed", i);
	}

	g454_check_values(test, G454_FILE, "client");
	g454_check_values(test, G454_SERVER, "server");

	/* and none of them collapsed into another: the list has them all */
	list = kunit_kzalloc(test, XATTR_LIST_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, list);
	len = xfs_listxattr(G454_FILE, list, XATTR_LIST_MAX);
	KUNIT_ASSERT_GT_MSG(test, len, 0, "listxattr returned %zd", len);
	for (off = 0; off < len; off += strlen(list + off) + 1)
		count++;
	KUNIT_EXPECT_EQ_MSG(test, count, (int)ARRAY_SIZE(g454_keys),
			    "listxattr returned %d names for %d keys", count,
			    (int)ARRAY_SIZE(g454_keys));
}

static int g454_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g454_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g454_cases[] = {
	KUNIT_CASE(keys_that_look_alike_stay_different_keys),
	{}
};

static struct kunit_suite g454_suite = {
	.name		= "xfstests/generic/454",
	.suite_init	= g454_suite_init,
	.suite_exit	= g454_suite_exit,
	.test_cases	= g454_cases,
};

kunit_test_suites(&g454_suite);

MODULE_DESCRIPTION("xfstests generic/454 over a loopback NFS mount");
MODULE_LICENSE("GPL");
