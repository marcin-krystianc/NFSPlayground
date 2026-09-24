// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/676 over a loopback NFS mount: seeking a directory to
 * valid and invalid positions.
 *
 * src/t_readdir_3 <dir> 4000 $RANDOM seeds lib/random.c's random() and
 * creates 4000 files with names of 8 to 69 random lowercase letters,
 * retrying a name that exists. It then runs one test twice, first through
 * opendir/readdir/telldir/seekdir, then through getdents64 and lseek on a
 * plain descriptor. The test reads 4000 entries one at a time, recording
 * the position each came from and seeking to its d_off before the next.
 * It seeks back to 4000 recorded positions picked at random, each of
 * which must give the same inode, name and (unless some entry was
 * DT_UNKNOWN) d_type. Last, it seeks to 4000 random positions below the
 * largest recorded one; an error there is ignored, but EOF is not. It is
 * the regression test for a48fc69fe658 ("udf: Fix crash after seekdir").
 *
 * Over NFS a directory offset is not an offset at all: it is a cookie the
 * server chose, cached by the client in its readdir pages. Seeking to a
 * cookie the client no longer has cached means going back to the server
 * with READDIR from that cookie, and seeking to a cookie the server never
 * issued means the client has to cope with whatever comes back.
 *
 * Every read follows a seek, which empties glibc's buffer, so each libc
 * read is one getdents64 into glibc's readdir buffer with the first entry
 * taken; xfs_libc_dirbuf() gives that buffer's size. The getdents pass
 * uses t_readdir_3's buffer, NAME_MAX + 1 + sizeof(struct linux_dirent64).
 * The seed is logged, as upstream logs it to $seqres.full.
 *
 * Not in upstream: after the random seeks, the directory must still
 * enumerate every entry from position 0.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include "xfstests_nfs_fixture.h"

#define G676_ROOT	XFS_MNT "/g676"
#define G676_DIR	G676_ROOT "/676-dir"

#define G676_COUNT	4000	/* files=4000 */
#define G676_MIN_NAME	8
#define G676_MAX_NAME	70
/* NAME_MAX + 1 + sizeof(struct linux_dirent64), 24 with its padding */
#define G676_KERNEL_BUF	(NAME_MAX + 1 + 24)
/* the whole directory: no getdents64 call can return more */
#define G676_MAXENTS	(G676_COUNT + 2)

struct g676 {
	struct kunit		*test;
	struct xfs_random	rnd;
	struct xfs_dirent	*ents;
	struct file		*d;
	size_t			bufsize;
	bool			ignore_error;
	bool			ignore_dtype;
};

/*
 * getentry: the first record of one getdents64 call. False where upstream
 * exits: on EOF, and on an error unless errors are being ignored.
 */
static bool g676_getentry(struct g676 *g, struct xfs_dirent *entry)
{
	int n = xfs_getdents(g->d, g->ents, G676_MAXENTS, g->bufsize);

	if (n < 0) {
		if (g->ignore_error)
			return true;
		KUNIT_FAIL(g->test, "getdents64: %d", n);
		return false;
	}
	if (n == 0) {
		KUNIT_FAIL(g->test, "Unexpected EOF while reading dir.");
		return false;
	}
	*entry = g->ents[0];
	/* NFS may or may not set d_type, depending on READDIRPLUS */
	if (entry->type == DT_UNKNOWN)
		g->ignore_dtype = true;
	return true;
}

static void g676_setpos(struct g676 *g, loff_t pos)
{
	vfs_llseek(g->d, pos, SEEK_SET);
}

static void g676_test(struct g676 *g, bool libc, const char *what)
{
	struct kunit *test = g->test;
	struct xfs_dirent *dbuf, entry;
	loff_t *pbuf, dpos, maxpos = 0;
	int i, n, pos;

	dbuf = kvcalloc(G676_COUNT, sizeof(*dbuf), GFP_KERNEL);
	pbuf = kvcalloc(G676_COUNT, sizeof(*pbuf), GFP_KERNEL);
	g->ents = kvcalloc(G676_MAXENTS, sizeof(*g->ents), GFP_KERNEL);
	if (!dbuf || !pbuf || !g->ents) {
		KUNIT_FAIL(test, "Out of memory for buffers.");
		goto out;
	}
	g->d = filp_open(G676_DIR, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(g->d)) {
		KUNIT_FAIL(test, "Cannot open dir: %ld", PTR_ERR(g->d));
		goto out;
	}
	g->bufsize = libc ? xfs_libc_dirbuf(g->d) : G676_KERNEL_BUF;
	if (!g->bufsize) {
		KUNIT_FAIL(test, "Cannot stat dir");
		goto close;
	}

	for (i = 0; i < G676_COUNT; i++) {
		pbuf[i] = g->d->f_pos;
		if (pbuf[i] > maxpos)
			maxpos = pbuf[i];
		if (!g676_getentry(g, dbuf + i))
			goto close;
		g676_setpos(g, dbuf[i].d_off);
	}

	for (i = 0; i < G676_COUNT; i++) {
		pos = xfs_random(&g->rnd) % G676_COUNT;
		g676_setpos(g, pbuf[pos]);
		if (!g676_getentry(g, &entry))
			goto close;
		if (dbuf[pos].ino != entry.ino ||
		    (!g->ignore_dtype && dbuf[pos].type != entry.type) ||
		    strcmp(dbuf[pos].name, entry.name)) {
			KUNIT_FAIL(test, "%s: Mismatch in dir entry %d at pos %llu",
				   what, pos, (unsigned long long)pbuf[pos]);
			goto close;
		}
	}

	g->ignore_error = true;
	for (i = 0; i < G676_COUNT; i++) {
		dpos = xfs_random(&g->rnd) % maxpos;
		g676_setpos(g, dpos);
		/* We don't care about the result but the kernel should not crash. */
		if (!g676_getentry(g, &entry)) {
			kunit_info(test, "%s: EOF at random position %lld",
				   what, dpos);
			goto close;
		}
	}
	g->ignore_error = false;

	/* not upstream: the walk from 0 still returns every entry */
	g676_setpos(g, 0);
	for (i = 0; (n = xfs_getdents(g->d, g->ents, G676_MAXENTS,
				      g->bufsize)) > 0; )
		i += n;
	KUNIT_EXPECT_EQ_MSG(test, n, 0, "%s: getdents64 from 0: %d", what, n);
	KUNIT_EXPECT_EQ_MSG(test, i, G676_COUNT + 2,
			    "%s: after the random seeks the directory lists %d entries",
			    what, i);
close:
	filp_close(g->d, NULL);
out:
	kvfree(g->ents);
	kvfree(pbuf);
	kvfree(dbuf);
}

static void g676_remove_tree(void *unused)
{
	struct xfs_dirent *ents;
	struct file *d;
	char *path;
	int n, i, removed;

	xfs_settle_fput();
	ents = kvcalloc(G676_MAXENTS, sizeof(*ents), GFP_KERNEL);
	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!ents || !path)
		goto out;
	do {
		removed = 0;
		d = filp_open(G676_DIR, O_RDONLY | O_DIRECTORY, 0);
		if (IS_ERR(d))
			break;
		while ((n = xfs_getdents(d, ents, G676_MAXENTS,
					 G676_MAXENTS * 24)) > 0)
			for (i = 0; i < n; i++) {
				if (!strcmp(ents[i].name, ".") ||
				    !strcmp(ents[i].name, ".."))
					continue;
				snprintf(path, PATH_MAX, G676_DIR "/%s",
					 ents[i].name);
				removed += !xfs_unlink(path);
			}
		filp_close(d, NULL);
	} while (removed);
	xfs_rmdir_settled(G676_DIR);
	xfs_rmdir_settled(G676_ROOT);
out:
	kfree(path);
	kvfree(ents);
}

static void seeking_a_directory_returns_the_same_entries(struct kunit *test)
{
	struct g676 g = { .test = test };
	char name[G676_MAX_NAME + 1], *path;
	unsigned int seed = get_random_u32_below(32768);	/* $RANDOM */
	struct file *f;
	int i, j, len;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G676_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g676_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G676_DIR), 0);
	path = kunit_kmalloc(test, PATH_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);

	kunit_info(test, "Using seed %u\n", seed);
	xfs_srandom(&g.rnd, seed);

	/* create_dir */
	for (i = 0; i < G676_COUNT; i++) {
		len = xfs_random(&g.rnd) % (G676_MAX_NAME - G676_MIN_NAME) +
		      G676_MIN_NAME;
		for (j = 0; j < len; j++)
			name[j] = xfs_random(&g.rnd) % 26 + 'a';
		name[len] = '\0';
		snprintf(path, PATH_MAX, G676_DIR "/%s", name);
		f = filp_open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
		if (IS_ERR(f) && PTR_ERR(f) == -EEXIST) {
			i--;		/* Try again */
			continue;
		}
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f),
				       "File creation failed: %ld", PTR_ERR(f));
		filp_close(f, NULL);
	}

	/* Testing readdir... */
	g676_test(&g, true, "readdir");
	/* Testing getdents... */
	g676_test(&g, false, "getdents");
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
