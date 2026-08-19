// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/360 over a loopback NFS mount: a very long symlink target.
 *
 * Upstream builds one target too long to live inside the inode --
 * four 254-character components joined by slashes, 1019 bytes -- creates a
 * symlink to it, and checks the content that comes back:
 *
 *	FNAME=$(perl -e 'print "a"x254')
 *	ln -s $FNAME/$FNAME/$FNAME/$FNAME $linkfile
 *	readlink $linkfile | md5sum
 *
 * The md5 is only there because 1019 characters are unwieldy in a golden
 * image; the assertion is that readlink returns exactly the string that was
 * stored. The target names nothing that exists and is never resolved.
 *
 * Over NFS the string travels in SYMLINK's linkdata and comes back in
 * READLINK, so a truncation or an off-by-one at the page boundary shows up
 * as a mismatch here. The port compares the returned target byte for byte
 * and checks the symlink's reported size, which is that same length.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>

#include "xfstests_nfs_fixture.h"

#define G360_ROOT	XFS_MNT "/g360"
#define G360_LINK	G360_ROOT "/360.symlink"

#define G360_COMP	254			/* "a"x254 */
#define G360_COMPS	4			/* $FNAME/$FNAME/$FNAME/$FNAME */
#define G360_LEN	(G360_COMPS * G360_COMP + (G360_COMPS - 1))

static void g360_remove_tree(void *unused)
{
	xfs_unlink(G360_LINK);
	xfs_rmdir(G360_ROOT);
}

static void long_symlink_targets_round_trip(struct kunit *test)
{
	struct kstat st;
	char *target, *back;
	ssize_t n;
	int i, c;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G360_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g360_remove_tree, NULL),
			0);

	target = kunit_kzalloc(test, G360_LEN + 1, GFP_KERNEL);
	back = kunit_kzalloc(test, G360_LEN + 2, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, target);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, back);

	for (c = 0, i = 0; c < G360_COMPS; c++) {
		if (c)
			target[i++] = '/';
		memset(target + i, 'a', G360_COMP);
		i += G360_COMP;
	}
	target[i] = '\0';
	KUNIT_ASSERT_EQ(test, i, G360_LEN);

	KUNIT_ASSERT_EQ_MSG(test, xfs_symlink(target, G360_LINK), 0,
			    "creating a %d-byte symlink target failed", G360_LEN);

	/* readlink: the same string, exactly */
	n = xfs_readlink(G360_LINK, back, G360_LEN + 2);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G360_LEN,
			    "READLINK returned %zd bytes for a %d-byte target",
			    n, G360_LEN);
	for (i = 0; i < G360_LEN; i++)
		if (back[i] != target[i]) {
			KUNIT_FAIL(test,
				   "the target came back wrong at byte %d: '%c' not '%c'",
				   i, back[i], target[i]);
			return;
		}

	/* the symlink inode's size is that length too */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G360_LINK, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G360_LEN,
			    "the symlink reports size %lld, not %d", st.size,
			    G360_LEN);
}

static int g360_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g360_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g360_cases[] = {
	KUNIT_CASE(long_symlink_targets_round_trip),
	{}
};

static struct kunit_suite g360_suite = {
	.name		= "xfstests/generic/360",
	.suite_init	= g360_suite_init,
	.suite_exit	= g360_suite_exit,
	.test_cases	= g360_cases,
};

kunit_test_suites(&g360_suite);

MODULE_DESCRIPTION("xfstests generic/360 over a loopback NFS mount");
MODULE_LICENSE("GPL");
