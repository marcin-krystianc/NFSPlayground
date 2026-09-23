// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/676 over a loopback NFS mount: seeking a directory to
 * valid and invalid positions.
 *
 * src/t_readdir_3 fills a directory with randomly named files, walks it
 * one entry at a time recording the offset each entry was read from, then
 * seeks back to those offsets in random order and requires the same entry
 * to come back each time. Finally it seeks to random offsets that were
 * never handed out and requires the kernel not to crash. It is the
 * regression test for a48fc69fe658 ("udf: Fix crash after seekdir").
 *
 * Over NFS a directory offset is not an offset at all: it is a cookie the
 * server chose, cached by the client in its readdir pages. Seeking to a
 * cookie the client no longer has cached means going back to the server
 * with READDIR from that cookie, and seeking to a cookie the server never
 * issued means the client has to cope with whatever comes back. Both are
 * exercised here; the port also keeps upstream's rule that an invalid
 * position may fail but may not corrupt the walk.
 *
 * Deviations: 200 entries rather than 4000 -- every one of them is a
 * CREATE RPC -- and a fixed seed, so a failure is reproducible. The names
 * are the same shape as upstream's (8 to 70 lowercase letters).
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/prandom.h>
#include <linux/slab.h>

#include "xfstests_nfs_fixture.h"

#define G676_ROOT	XFS_MNT "/g676"
#define G676_DIR	G676_ROOT "/676-dir"

#define G676_COUNT	200
#define G676_MIN_NAME	8
#define G676_MAX_NAME	70
#define G676_SEED	20260922

struct g676_entry {
	loff_t	pos;			/* offset this entry was read from */
	u64	ino;
	char	name[G676_MAX_NAME + 1];
};

struct g676_iter {
	struct dir_context	ctx;
	struct g676_entry	*out;
	bool			taken;
};

static bool g676_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g676_iter *it = container_of(ctx, struct g676_iter, ctx);

	if (it->taken)
		return false;		/* stop before consuming this one */
	if (len > G676_MAX_NAME)
		len = G676_MAX_NAME;
	memcpy(it->out->name, name, len);
	it->out->name[len] = '\0';
	it->out->ino = ino;
	it->taken = true;
	return true;
}

/* read exactly one entry from wherever the directory is positioned */
static bool g676_one(struct file *d, struct g676_entry *out)
{
	struct g676_iter it = { .ctx.actor = g676_actor, .out = out };

	out->pos = d->f_pos;
	if (iterate_dir(d, &it.ctx) < 0)
		return false;
	return it.taken;
}

static char g676_names[G676_COUNT][G676_MAX_NAME + 1];

static void g676_remove_tree(void *unused)
{
	char path[128];
	int i;

	for (i = 0; i < G676_COUNT; i++) {
		if (!g676_names[i][0])
			continue;
		snprintf(path, sizeof(path), G676_DIR "/%s", g676_names[i]);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G676_DIR);
	xfs_rmdir_settled(G676_ROOT);
}

static void seeking_a_directory_returns_the_same_entries(struct kunit *test)
{
	struct rnd_state rnd;
	struct g676_entry *seen, one;
	char path[128];
	struct file *d;
	loff_t maxpos = 0;
	int i, j, len, nseen = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G676_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g676_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G676_DIR), 0);

	seen = kunit_kcalloc(test, G676_COUNT + 2, sizeof(*seen), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, seen);

	prandom_seed_state(&rnd, G676_SEED);
	memset(g676_names, 0, sizeof(g676_names));
	for (i = 0; i < G676_COUNT; i++) {
		len = G676_MIN_NAME +
		      prandom_u32_state(&rnd) % (G676_MAX_NAME - G676_MIN_NAME);
		for (j = 0; j < len; j++)
			g676_names[i][j] = 'a' + prandom_u32_state(&rnd) % 26;
		g676_names[i][len] = '\0';
		snprintf(path, sizeof(path), G676_DIR "/%s", g676_names[i]);
		if (xfs_exists(path)) {
			i--;		/* upstream retries on EEXIST */
			continue;
		}
		KUNIT_ASSERT_EQ_MSG(test, xfs_write_new_file(path, "", 0), 0,
				    "creating entry %d failed", i);
	}

	d = filp_open(G676_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "open: %ld", PTR_ERR(d));

	/* walk it, recording the position each entry came from */
	while (nseen < G676_COUNT + 2 && g676_one(d, &seen[nseen])) {
		if (seen[nseen].pos > maxpos)
			maxpos = seen[nseen].pos;
		nseen++;
	}
	KUNIT_ASSERT_EQ_MSG(test, nseen, G676_COUNT + 2,
			    "the walk returned %d entries, expected %d",
			    nseen, G676_COUNT + 2);

	/* seek back to each recorded position, in random order */
	for (i = 0; i < nseen; i++) {
		int pos = prandom_u32_state(&rnd) % nseen;

		KUNIT_ASSERT_EQ_MSG(test,
				    vfs_llseek(d, seen[pos].pos, SEEK_SET),
				    seen[pos].pos,
				    "seeking to %lld failed", seen[pos].pos);
		KUNIT_ASSERT_TRUE_MSG(test, g676_one(d, &one),
				      "no entry at recorded position %lld",
				      seen[pos].pos);
		KUNIT_ASSERT_STREQ_MSG(test, one.name, seen[pos].name,
				       "position %lld gave a different name",
				       seen[pos].pos);
		KUNIT_EXPECT_EQ_MSG(test, one.ino, seen[pos].ino,
				    "position %lld gave a different inode",
				    seen[pos].pos);
	}

	/*
	 * And positions that were never handed out. The result is not
	 * defined -- upstream ignores it -- but the walk must survive, so
	 * the directory is enumerated once more afterwards.
	 */
	for (i = 0; i < nseen; i++) {
		loff_t dpos = maxpos ? prandom_u32_state(&rnd) % maxpos : 0;

		vfs_llseek(d, dpos, SEEK_SET);
		g676_one(d, &one);
	}

	KUNIT_ASSERT_EQ(test, vfs_llseek(d, 0, SEEK_SET), 0LL);
	i = 0;
	while (g676_one(d, &one))
		i++;
	KUNIT_EXPECT_EQ_MSG(test, i, nseen,
			    "after the random seeks the directory enumerates %d entries, expected %d",
			    i, nseen);

	filp_close(d, NULL);
}

static int g676_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g676_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g676_cases[] = {
	KUNIT_CASE_SLOW(seeking_a_directory_returns_the_same_entries),
	{}
};

static struct kunit_suite g676_suite = {
	.name		= "xfstests/generic/676",
	.suite_init	= g676_suite_init,
	.suite_exit	= g676_suite_exit,
	.test_cases	= g676_cases,
};

kunit_test_suites(&g676_suite);

MODULE_DESCRIPTION("xfstests generic/676 over a loopback NFS mount");
MODULE_LICENSE("GPL");
