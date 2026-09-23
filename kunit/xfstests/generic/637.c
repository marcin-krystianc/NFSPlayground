// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/637 over a loopback NFS mount: a directory modified
 * while it is being read.
 *
 * src/t_dir_offset2 opens a directory, reads one small getdents batch,
 * and then creates or unlinks an entry before finishing the walk. The
 * old handle is allowed to miss a new entry or return a stale one -- that
 * is what POSIX permits -- but a handle opened *after* the change must
 * see the directory as it is now. It also requires every entry's d_off to
 * be unique, and seeking back to a recorded d_off to return the entry
 * that followed it. generic/637 runs it once on an empty directory and
 * then ten times while unlinking entries from a hundred.
 *
 * Over NFS every one of those is a question about cookies: d_off is the
 * server's cookie, the client caches whole pages of entries keyed on it,
 * and a modification bumps the directory's change attribute, which is
 * what makes a newly opened handle refill from the server. A cookie that
 * repeats, or a fresh handle that serves the old cached pages, is the
 * failure this catches.
 *
 * Deviations: entries are read one at a time rather than in 200-byte
 * getdents batches, which is the same thing at a finer grain; 32 files
 * rather than 100, and two mutations rather than eleven.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G637_ROOT	XFS_MNT "/g637"
#define G637_DIR	G637_ROOT "/test-637"
#define G637_FILES	32
#define G637_MAX	(G637_FILES + 8)

struct g637_entry {
	loff_t	off;		/* the cookie this entry was read at */
	u64	ino;
	char	name[NAME_MAX + 1];
};

struct g637_iter {
	struct dir_context	ctx;
	struct g637_entry	*out;
	bool			taken;
};

static bool g637_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g637_iter *it = container_of(ctx, struct g637_iter, ctx);

	if (it->taken)
		return false;
	if (len > NAME_MAX)
		len = NAME_MAX;
	memcpy(it->out->name, name, len);
	it->out->name[len] = '\0';
	it->out->ino = ino;
	it->taken = true;
	return true;
}

static bool g637_one(struct file *d, struct g637_entry *out)
{
	struct g637_iter it = { .ctx.actor = g637_actor, .out = out };

	if (iterate_dir(d, &it.ctx) < 0)
		return false;
	out->off = d->f_pos;	/* the cookie that follows this entry */
	return it.taken;
}

static void g637_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < G637_FILES; i++) {
		snprintf(path, sizeof(path), G637_DIR "/%d", i);
		xfs_unlink(path);
	}
	xfs_unlink(G637_DIR "/newfile");
	xfs_rmdir_settled(G637_DIR);
	xfs_rmdir_settled(G637_ROOT);
}

static int g637_setup(struct kunit *test)
{
	char path[64];
	int i, err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G637_ROOT), 0);
	err = kunit_add_action_or_reset(test, g637_remove_tree, NULL);
	if (err)
		return err;
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G637_DIR), 0);
	for (i = 0; i < G637_FILES; i++) {
		snprintf(path, sizeof(path), G637_DIR "/%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(path, "", 0), 0);
	}
	/*
	 * Every one of those was opened and closed, and an in-kernel close
	 * defers the final fput. Unlinking a file whose struct file is still
	 * alive sillyrenames it, which would put a transient .nfsXXXX entry
	 * in the directory in the middle of the walk below -- and then
	 * remove it again when the fput landed, which is a directory change
	 * this test is not making. Settle first.
	 */
	xfs_settle_fput();
	return 0;
}

/* walk a directory to the end, recording every entry */
static int g637_walk(struct kunit *test, struct file *d,
		     struct g637_entry *seen, int max)
{
	int n = 0;

	while (n < max && g637_one(d, &seen[n])) {
		/* a sillyrename here is the fixture racing itself, not a
		 * directory change the test made: name it rather than let
		 * it surface as a confusing cookie mismatch later
		 */
		KUNIT_EXPECT_NE_MSG(test, strncmp(seen[n].name, ".nfs", 4), 0,
				    "a sillyrenamed entry (%s) appeared during the walk",
				    seen[n].name);
		n++;
	}
	return n;
}

static void g637_check_offsets(struct kunit *test, struct g637_entry *seen,
			       int n, const char *what)
{
	int i, j;

	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			KUNIT_EXPECT_NE_MSG(test, seen[i].off, seen[j].off,
					    "%s: entries %d and %d share the cookie %lld",
					    what, i, j, seen[i].off);
}

static void g637_mutation(struct kunit *test, bool create)
{
	const char *what = create ? "after a create" : "after an unlink";
	struct g637_entry *seen, one;
	struct file *d, *d2;
	int n, i, found = 0;

	KUNIT_ASSERT_EQ(test, g637_setup(test), 0);

	seen = kunit_kcalloc(test, G637_MAX, sizeof(*seen), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, seen);

	d = filp_open(G637_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));

	/* one entry, then change the directory underneath the walk */
	KUNIT_ASSERT_TRUE_MSG(test, g637_one(d, &seen[0]),
			      "the directory returned nothing at all");
	if (create)
		KUNIT_ASSERT_EQ(test,
				xfs_write_new_file(G637_DIR "/newfile", "", 0),
				0);
	else
		KUNIT_ASSERT_EQ(test, xfs_unlink(G637_DIR "/0"), 0);

	/* the old handle may miss it or return it: only finish the walk */
	n = 1 + g637_walk(test, d, seen + 1, G637_MAX - 1);
	g637_check_offsets(test, seen, n, "the old handle");

	/* a handle opened after the change must see the new state */
	d2 = filp_open(G637_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d2), "reopen: %ld", PTR_ERR(d2));
	n = g637_walk(test, d2, seen, G637_MAX);
	g637_check_offsets(test, seen, n, "the new handle");
	for (i = 0; i < n; i++)
		if (!strcmp(seen[i].name, create ? "newfile" : "0"))
			found++;
	KUNIT_EXPECT_EQ_MSG(test, found, create ? 1 : 0,
			    "%s: the entry was found %d times on a handle opened afterwards",
			    what, found);

	/* seeking back to a recorded cookie returns the entry after it */
	for (i = n - 1; i > 0; i--) {
		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_llseek(d2, seen[i - 1].off, SEEK_SET),
				    seen[i - 1].off,
				    "%s: seeking to %lld failed", what,
				    seen[i - 1].off);
		KUNIT_ASSERT_TRUE_MSG(test, g637_one(d2, &one),
				      "%s: nothing at cookie %lld", what,
				      seen[i - 1].off);
		KUNIT_EXPECT_STREQ_MSG(test, one.name, seen[i].name,
				       "%s: cookie %lld gave a different entry",
				       what, seen[i - 1].off);
	}

	filp_close(d2, NULL);
	filp_close(d, NULL);
}

static void a_create_during_a_walk(struct kunit *test)
{
	g637_mutation(test, true);
}

static void an_unlink_during_a_walk(struct kunit *test)
{
	g637_mutation(test, false);
}

static int g637_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g637_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g637_cases[] = {
	KUNIT_CASE(a_create_during_a_walk),
	KUNIT_CASE(an_unlink_during_a_walk),
	{}
};

static struct kunit_suite g637_suite = {
	.name		= "xfstests/generic/637",
	.suite_init	= g637_suite_init,
	.suite_exit	= g637_suite_exit,
	.test_cases	= g637_cases,
};

kunit_test_suites(&g637_suite);

MODULE_DESCRIPTION("xfstests generic/637 over a loopback NFS mount");
MODULE_LICENSE("GPL");
