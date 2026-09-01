// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/257 over a loopback NFS mount: readdir offset stability.
 *
 * Upstream's t_dir_offset2 regression: reading a directory with getdents
 * in small batches -- closing, reopening and seeking back to the saved
 * offset between batches -- must enumerate every entry exactly once, no
 * duplicates, no holes. Over NFS a directory position is a server READDIR
 * cookie, so what is really under test is cookie save/restore through the
 * client's readdir caching: 168 entries, batches of 7, a reopen + llseek
 * to the saved position each round, plus a rewind-to-zero pass.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G257_ROOT	XFS_MNT "/g257"
#define G257_ENTRIES	168
#define G257_BATCH	7

static u8 g257_seen[G257_ENTRIES];

struct g257_iter {
	struct dir_context	ctx;
	struct kunit		*test;
	int			taken;
	int			dup;
	int			alien;
};

static bool g257_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g257_iter *it = container_of(ctx, struct g257_iter, ctx);
	char nbuf[16];
	int idx;

	if (it->taken >= G257_BATCH)
		return false;	/* stop; the current entry is not consumed */

	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;

	if (len < 2 || len >= sizeof(nbuf) || name[0] != 'e') {
		it->alien++;
		return true;
	}
	memcpy(nbuf, name + 1, len - 1);
	nbuf[len - 1] = '\0';
	if (kstrtoint(nbuf, 10, &idx) || idx < 0 || idx >= G257_ENTRIES) {
		it->alien++;
		return true;
	}
	if (g257_seen[idx]++)
		it->dup++;
	it->taken++;
	return true;
}

static void g257_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G257_ENTRIES; i++) {
		snprintf(buf, sizeof(buf), G257_ROOT "/e%d", i);
		xfs_unlink(buf);
	}
	xfs_rmdir(G257_ROOT);
}

/* one full enumeration in batches, reopening + seeking between batches */
static void g257_enumerate(struct kunit *test)
{
	struct file *d;
	loff_t pos = 0;
	int rounds;

	memset(g257_seen, 0, sizeof(g257_seen));

	/* generously bounded: 168/7 = 24 data rounds plus slack */
	for (rounds = 0; rounds < 200; rounds++) {
		struct g257_iter it = {
			.ctx.actor = g257_actor,
			.test = test,
		};

		d = filp_open(G257_ROOT, O_RDONLY | O_DIRECTORY, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(d));
		KUNIT_ASSERT_EQ_MSG(test, vfs_llseek(d, pos, SEEK_SET), pos,
				    "seeking the directory to %lld failed",
				    pos);
		KUNIT_ASSERT_EQ(test, iterate_dir(d, &it.ctx), 0);
		pos = d->f_pos;	/* the cookie to resume from */
		filp_close(d, NULL);

		KUNIT_ASSERT_EQ_MSG(test, it.dup, 0,
				    "round %d returned duplicate entries",
				    rounds);
		KUNIT_ASSERT_EQ_MSG(test, it.alien, 0,
				    "round %d returned unexpected names",
				    rounds);
		if (it.taken == 0)
			break;	/* EOF round */
	}
	KUNIT_ASSERT_LT_MSG(test, rounds, 200,
			    "the directory never reached EOF");
}

static void readdir_batches_enumerate_exactly_once(struct kunit *test)
{
	char buf[64];
	int i, missing;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G257_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g257_remove_tree,
						  NULL), 0);

	for (i = 0; i < G257_ENTRIES; i++) {
		snprintf(buf, sizeof(buf), G257_ROOT "/e%d", i);
		KUNIT_ASSERT_EQ(test, xfs_write_new_file(buf, "x", 1), 0);
	}

	g257_enumerate(test);
	for (i = 0, missing = 0; i < G257_ENTRIES; i++)
		if (!g257_seen[i])
			missing++;
	KUNIT_EXPECT_EQ_MSG(test, missing, 0,
			    "%d of %d entries never enumerated", missing,
			    G257_ENTRIES);

	/* a second full pass (rewound to zero) must behave identically */
	g257_enumerate(test);
	for (i = 0, missing = 0; i < G257_ENTRIES; i++)
		if (!g257_seen[i])
			missing++;
	KUNIT_EXPECT_EQ_MSG(test, missing, 0,
			    "the rewound pass lost %d entries", missing);
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
	KUNIT_CASE(readdir_batches_enumerate_exactly_once),
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
