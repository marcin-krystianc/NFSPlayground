// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/736 over a loopback NFS mount: readdir while the
 * entries are being renamed.
 *
 * src/readdir-while-renames fills a directory with 5000 files, opens it,
 * and then for every entry readdir returns it renames that entry away and
 * back again before asking for the next one. Renaming an entry can make
 * it appear again, so the walk is allowed to return more entries than
 * exist -- upstream accepts anything from NUM_FILES + 2 up to 3 *
 * NUM_FILES -- but it must terminate. btrfs looped forever (commit
 * 9b378f6ad48c, "btrfs: fix infinite directory reads").
 *
 * Over NFS the same question lands on the client's readdir cache and the
 * server's cookies: each rename changes the directory, so the client's
 * cached pages are invalidated and the next getdents goes back to the
 * server with the last cookie. If the server's cookie for a renamed entry
 * sends the client back to an earlier point, the walk never ends. The
 * port keeps upstream's bounds and its one-entry-at-a-time shape.
 *
 * Deviations: 500 files rather than 5000. Every entry costs two RENAME
 * RPCs plus a READDIR, and the property does not depend on the count.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G736_ROOT	XFS_MNT "/g736"
#define G736_DIR	G736_ROOT "/testdir"

#define G736_FILES	500
#define G736_MAX	(3 * G736_FILES)

struct g736_iter {
	struct dir_context	ctx;
	char			name[NAME_MAX + 1];
	bool			taken;
	bool			dot;
};

static bool g736_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g736_iter *it = container_of(ctx, struct g736_iter, ctx);

	if (it->taken)
		return false;
	if (len > NAME_MAX)
		len = NAME_MAX;
	memcpy(it->name, name, len);
	it->name[len] = '\0';
	it->dot = !strcmp(it->name, ".") || !strcmp(it->name, "..");
	it->taken = true;
	return true;
}

static void g736_remove_tree(void *unused)
{
	char path[64];
	int i;

	snprintf(path, sizeof(path), G736_DIR "/TEMPFILE");
	xfs_unlink(path);
	for (i = 1; i <= G736_FILES; i++) {
		snprintf(path, sizeof(path), G736_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G736_DIR);
	xfs_rmdir_settled(G736_ROOT);
}

static void renaming_every_entry_still_ends_the_walk(struct kunit *test)
{
	char path[64], tmp[64], cur[64];
	struct file *d;
	int i, count = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G736_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g736_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G736_DIR), 0);

	for (i = 1; i <= G736_FILES; i++) {
		snprintf(path, sizeof(path), G736_DIR "/%d", i);
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(path, "", 0), 0,
				    "creating file %d failed", i);
	}

	snprintf(tmp, sizeof(tmp), G736_DIR "/TEMPFILE");

	d = filp_open(G736_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));

	for (;;) {
		struct g736_iter it = { .ctx.actor = g736_actor };

		KUNIT_ASSERT_EQ(test, iterate_dir(d, &it.ctx), 0);
		if (!it.taken)
			break;

		count++;
		KUNIT_ASSERT_LE_MSG(test, count, G736_MAX,
				    "the walk returned more than %d entries; it is not terminating",
				    G736_MAX);
		if (it.dot)
			continue;

		snprintf(cur, sizeof(cur), G736_DIR "/%s", it.name);
		KUNIT_ASSERT_EQ_MSG(test, xfs_rename(cur, tmp), 0,
				    "renaming %s to TEMPFILE failed", it.name);
		KUNIT_ASSERT_EQ_MSG(test, xfs_rename(tmp, cur), 0,
				    "renaming TEMPFILE back to %s failed",
				    it.name);
	}
	filp_close(d, NULL);

	KUNIT_EXPECT_GE_MSG(test, count, G736_FILES + 2,
			    "the walk returned %d entries, fewer than the %d that exist",
			    count, G736_FILES + 2);
}

static int g736_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g736_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g736_cases[] = {
	KUNIT_CASE_SLOW(renaming_every_entry_still_ends_the_walk),
	{}
};

static struct kunit_suite g736_suite = {
	.name		= "xfstests/generic/736",
	.suite_init	= g736_suite_init,
	.suite_exit	= g736_suite_exit,
	.test_cases	= g736_cases,
};

kunit_test_suites(&g736_suite);

MODULE_DESCRIPTION("xfstests generic/736 over a loopback NFS mount");
MODULE_LICENSE("GPL");
