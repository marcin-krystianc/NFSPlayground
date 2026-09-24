// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/736 over a loopback NFS mount: readdir while the
 * entries are being renamed.
 *
 * src/readdir-while-renames creates testdir with files 1 to 5000, opens
 * it, and for every entry readdir(3) returns, other than "." and "..",
 * renames that entry to TEMPFILE and back before asking for the next
 * one. Renaming an entry can make it appear again, so the walk may
 * return more entries than exist: upstream accepts anything from
 * NUM_FILES + 2 up to 3 * NUM_FILES, but the walk must end. btrfs looped
 * forever (commit 9b378f6ad48c, "btrfs: fix infinite directory reads").
 *
 * readdir(3) hands out entries from a buffer that glibc fills with one
 * getdents64 call, so the renames run between calls, a batch at a time.
 * The port reads the same batches: getdents64 with xfs_libc_dirbuf()'s
 * size, entries taken in order, renames after each.
 *
 * Over NFS the same question lands on the client's readdir cache and the
 * server's cookies: each rename changes the directory, so the client's
 * cached pages are invalidated and the next getdents goes back to the
 * server with the last cookie. If the server's cookie for a renamed entry
 * sends the client back to an earlier point, the walk never ends.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include "xfstests_nfs_fixture.h"

#define G736_ROOT	XFS_MNT "/g736"
#define G736_DIR	G736_ROOT "/testdir"

#define G736_FILES	5000	/* NUM_FILES */
#define G736_MAX	(3 * G736_FILES)

static void g736_remove_tree(void *unused)
{
	char path[64];
	int i;

	xfs_settle_fput();
	xfs_unlink(G736_DIR "/TEMPFILE");
	for (i = 1; i <= G736_FILES; i++) {
		snprintf(path, sizeof(path), G736_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G736_DIR);
	xfs_rmdir_settled(G736_ROOT);
}

static void g736_kvfree(void *p)
{
	kvfree(p);
}

static void renaming_every_entry_still_ends_the_walk(struct kunit *test)
{
	char path[64], cur[64];
	struct xfs_dirent *ents;
	struct file *d;
	size_t bufsize;
	int i, n, count = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G736_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g736_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G736_DIR), 0);

	/* the walk fails past G736_MAX entries, so no batch needs more */
	ents = kvcalloc(G736_MAX + 1, sizeof(*ents), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, g736_kvfree,
							ents), 0);

	/* fopen(file_name, "w") */
	for (i = 1; i <= G736_FILES; i++) {
		struct file *f;

		snprintf(path, sizeof(path), G736_DIR "/%d", i);
		f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
				       "Failed to create file number %d: %ld",
				       i, PTR_ERR(f));
		filp_close(f, NULL);
	}
	xfs_settle_fput();

	d = filp_open(G736_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));
	bufsize = xfs_libc_dirbuf(d);
	KUNIT_ASSERT_GT(test, bufsize, 0UL);

	while ((n = xfs_getdents(d, ents, G736_MAX + 1, bufsize)) > 0) {
		for (i = 0; i < n; i++) {
			count++;
			if (count > G736_MAX) {
				KUNIT_FAIL(test,
					   "Found too many directory entries (%d)",
					   count);
				goto out;
			}
			/* Can't rename "." and "..", skip them. */
			if (!strcmp(ents[i].name, ".") ||
			    !strcmp(ents[i].name, ".."))
				continue;
			snprintf(cur, sizeof(cur), G736_DIR "/%s",
				 ents[i].name);
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_rename(cur, G736_DIR "/TEMPFILE"),
					    0, "Failed to rename '%s' to TEMPFILE",
					    ents[i].name);
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_rename(G736_DIR "/TEMPFILE", cur),
					    0, "Failed to rename TEMPFILE to '%s'",
					    ents[i].name);
		}
	}
	KUNIT_EXPECT_EQ_MSG(test, n, 0, "Failed to read directory: %d", n);
	KUNIT_EXPECT_GE_MSG(test, count, G736_FILES + 2,
			    "Found less directory entries than expected (%d but expected %d)",
			    count, G736_FILES + 2);
out:
	filp_close(d, NULL);
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
