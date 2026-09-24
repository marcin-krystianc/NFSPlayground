// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/257 over a loopback NFS mount: no duplicate d_off, and
 * every d_off seekable.
 *
 * Upstream touches 1 .. 168 in a fresh directory and runs
 * src/t_dir_offset2 on it with its default 4096-byte buffer: read the
 * whole directory with getdents64, fail if two entries report the same
 * d_off, then from the last entry back to the first, lseek to the d_off
 * of the entry before it (0 for the first) and require the next getdents
 * to start with that entry's inode.
 *
 * Over NFS a directory offset is a READDIR cookie, handed out by the
 * server and cached by the client in its readdir pages, so what is under
 * test is cookie uniqueness and cookie save/restore: every lseek back is
 * a position the client may no longer hold a page for.
 *
 * xfs_t_dir_offset2() is t_dir_offset2 with its checks as KUnit
 * expectations; xfs_getdents() sizes batches and sets d_off the way
 * getdents64 does.
 *
 * A second case, not in upstream, resumes from a saved position through a
 * fresh open instead of the same descriptor: batches of seven entries,
 * each read by a new open plus an lseek to the last d_off, must still
 * return every name exactly once.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G257_ROOT	XFS_MNT "/g257"
#define G257_DIR	G257_ROOT "/ttt"
#define G257_ENTRIES	168

static void g257_populate(struct kunit *test)
{
	char buf[64];
	int n;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G257_ROOT), 0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G257_DIR), 0);
	for (n = 1; n <= G257_ENTRIES; n++) {
		snprintf(buf, sizeof(buf), G257_DIR "/%d", n);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(buf, "", 0), 0);
	}
}

static void g257_remove_tree(void *unused)
{
	char buf[64];
	int n;

	for (n = 1; n <= G257_ENTRIES; n++) {
		snprintf(buf, sizeof(buf), G257_DIR "/%d", n);
		xfs_unlink(buf);
	}
	xfs_rmdir_settled(G257_DIR);
	xfs_rmdir_settled(G257_ROOT);
}

static void d_offs_are_unique_and_seekable(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g257_remove_tree,
						  NULL), 0);
	g257_populate(test);
	xfs_t_dir_offset2(test, G257_DIR, 4096, NULL);
}

/* not in upstream: resume each batch of seven through a fresh open */
static void batches_through_fresh_opens_see_every_name_once(struct kunit *test)
{
	struct xfs_dirent *ents;
	u8 *seen;
	loff_t pos = 0;
	int rounds, n, i, idx;

	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g257_remove_tree,
						  NULL), 0);
	g257_populate(test);
	ents = kunit_kcalloc(test, 7, sizeof(*ents), GFP_KERNEL);
	seen = kunit_kzalloc(test, G257_ENTRIES + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, seen);

	for (rounds = 0; rounds < 200; rounds++) {
		struct file *d = filp_open(G257_DIR, O_RDONLY | O_DIRECTORY, 0);

		KUNIT_ASSERT_FALSE(test, IS_ERR(d));
		KUNIT_ASSERT_EQ(test, vfs_llseek(d, pos, SEEK_SET), pos);
		n = xfs_getdents(d, ents, 7, 32768);
		filp_close(d, NULL);
		KUNIT_ASSERT_GE(test, n, 0);
		if (!n)
			break;
		pos = ents[n - 1].d_off;
		for (i = 0; i < n; i++) {
			if (!strcmp(ents[i].name, ".") ||
			    !strcmp(ents[i].name, ".."))
				continue;
			KUNIT_ASSERT_EQ_MSG(test, kstrtoint(ents[i].name, 10, &idx),
					    0, "unexpected name %s", ents[i].name);
			KUNIT_ASSERT_TRUE(test, idx >= 1 && idx <= G257_ENTRIES);
			KUNIT_EXPECT_EQ_MSG(test, seen[idx]++, 0,
					    "%d returned twice", idx);
		}
	}
	KUNIT_ASSERT_LT_MSG(test, rounds, 200, "the directory never ended");
	for (idx = 1; idx <= G257_ENTRIES; idx++)
		KUNIT_EXPECT_EQ_MSG(test, seen[idx], 1, "%d never returned", idx);
}

static int g257_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g257_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g257_cases[] = {
	KUNIT_CASE(d_offs_are_unique_and_seekable),
	KUNIT_CASE(batches_through_fresh_opens_see_every_name_once),
	{}
};

static struct kunit_suite g257_suite = {
	.name		= "xfstests/generic/257",
	.suite_init	= g257_suite_init,
	.suite_exit	= g257_suite_exit,
	.test_cases	= g257_cases,
};

kunit_test_suites(&g257_suite);

MODULE_DESCRIPTION("xfstests generic/257 over a loopback NFS mount");
MODULE_LICENSE("GPL");
