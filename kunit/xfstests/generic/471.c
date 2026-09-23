// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/471 over a loopback NFS mount: rewinddir must show
 * names created after the directory was opened.
 *
 * src/rewinddir-test opens a directory, creates new names in it, calls
 * rewinddir(3) and requires the following readdir(3) calls to return
 * them. POSIX says so explicitly: rewinddir "shall also cause the
 * directory stream to refer to the current state of the corresponding
 * directory". btrfs failed it by caching a last-index at open time.
 *
 * Over NFS rewinddir is an llseek to 0, and the names come from the
 * client's own readdir page cache, filled by READDIR RPCs and keyed on
 * cookies. So the question is whether seeking back to the start makes the
 * client re-read the directory or re-serve the pages it already had --
 * nfs_llseek_dir() clears the "directory is unchanged" state for exactly
 * this reason. The port adds the names through the same mount, so the
 * client's own change attribute moves; the entries must appear either
 * way.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G471_ROOT	XFS_MNT "/g471"
#define G471_DIR	G471_ROOT "/test-471"

#define G471_FIRST	8	/* names present when the directory is opened */
#define G471_ADDED	8	/* names created afterwards */
#define G471_TOTAL	(G471_FIRST + G471_ADDED)

struct g471_iter {
	struct dir_context	ctx;
	u8			seen[G471_TOTAL];
	int			alien;
	int			total;		/* including . and .. */
};

static bool g471_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g471_iter *it = container_of(ctx, struct g471_iter, ctx);
	char buf[16];
	int idx;

	it->total++;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (len < 2 || len >= sizeof(buf) || name[0] != 'e') {
		it->alien++;
		return true;
	}
	memcpy(buf, name + 1, len - 1);
	buf[len - 1] = '\0';
	if (kstrtoint(buf, 10, &idx) || idx < 0 || idx >= G471_TOTAL) {
		it->alien++;
		return true;
	}
	it->seen[idx]++;
	return true;
}

static void g471_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < G471_TOTAL; i++) {
		snprintf(path, sizeof(path), G471_DIR "/e%d", i);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G471_DIR);
	xfs_rmdir_settled(G471_ROOT);
}

static void g471_enumerate(struct kunit *test, struct file *d,
			   struct g471_iter *it, const char *when)
{
	int before;

	memset(it->seen, 0, sizeof(it->seen));
	it->alien = 0;
	it->total = 0;
	/* one iterate_dir() is one getdents(2): a batch, not the whole dir */
	do {
		before = it->total;
		KUNIT_ASSERT_EQ_MSG(test, iterate_dir(d, &it->ctx), 0,
				    "%s: iterate_dir failed", when);
	} while (it->total > before);
	KUNIT_EXPECT_EQ_MSG(test, it->alien, 0,
			    "%s: %d unexpected names", when, it->alien);
}

static void rewinddir_shows_names_created_after_opendir(struct kunit *test)
{
	struct g471_iter it = { .ctx.actor = g471_actor };
	char path[64];
	struct file *d;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G471_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g471_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G471_DIR), 0);

	for (i = 0; i < G471_FIRST; i++) {
		snprintf(path, sizeof(path), G471_DIR "/e%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	}

	/* opendir, and read it to the end so the client has cached it */
	d = filp_open(G471_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));

	g471_enumerate(test, d, &it, "first pass");
	for (i = 0; i < G471_FIRST; i++)
		KUNIT_EXPECT_EQ_MSG(test, it.seen[i], 1,
				    "first pass: e%d appeared %u times", i,
				    it.seen[i]);

	/* new names, created while the directory is open */
	for (i = G471_FIRST; i < G471_TOTAL; i++) {
		snprintf(path, sizeof(path), G471_DIR "/e%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	}

	/* rewinddir(3) */
	KUNIT_ASSERT_EQ_MSG(test, vfs_llseek(d, 0, SEEK_SET), 0LL,
			    "seeking the directory back to 0 failed");

	g471_enumerate(test, d, &it, "after rewind");
	for (i = 0; i < G471_TOTAL; i++)
		KUNIT_EXPECT_EQ_MSG(test, it.seen[i], 1,
				    "after rewind: e%d appeared %u times", i,
				    it.seen[i]);

	filp_close(d, NULL);
}

static int g471_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g471_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g471_cases[] = {
	KUNIT_CASE(rewinddir_shows_names_created_after_opendir),
	{}
};

static struct kunit_suite g471_suite = {
	.name		= "xfstests/generic/471",
	.suite_init	= g471_suite_init,
	.suite_exit	= g471_suite_exit,
	.test_cases	= g471_cases,
};

kunit_test_suites(&g471_suite);

MODULE_DESCRIPTION("xfstests generic/471 over a loopback NFS mount");
MODULE_LICENSE("GPL");
