// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/339 over a loopback NFS mount: 10000 directories with
 * names of up to 252 random bytes, read back after a mount cycle, removed.
 *
 * Upstream runs "src/dirhash_collide -d -n 10000" in a new directory on
 * SCRATCH_MNT, cycles the mount (checking the filesystem in between) and
 * "rm -rf"s the directory. dirhash_collide makes each name from 248
 * random non-zero bytes and chooses the last four so the XFS directory
 * hash of the whole name comes out fixed, which piles entries into XFS's
 * hash buckets; names containing '.' or '/' are redrawn, and a duplicate
 * name is ignored.
 *
 * The tmpfs export has no directory hash, so here the test is a large
 * directory of long names with arbitrary bytes: 10000 MKDIRs, then after
 * the mount cycle a cold READDIR of the whole directory -- many replies,
 * each resumed from a cookie -- and 10000 RMDIRs by the names READDIR
 * returned.
 *
 * Deviations: SCRATCH_MNT is the fixture's second mount of the export
 * (XFS_SCRATCH_MNT), cycled with xfs_scratch_umount()/xfs_scratch_mount();
 * there is no filesystem check to run. rm -rf's directory walk is
 * iterate_dir() until a pass adds nothing, then an rmdir per name, and the
 * port requires READDIR to return exactly the directories it created.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "xfstests_nfs_fixture.h"

#define G339_DIR	XFS_SCRATCH_MNT "/339.dir"
#define G339_N		10000
#define G339_NAMELEN	252

/* dirhash_collide.c's generator */
static u32 g339_rol32(u32 word, unsigned int shift)
{
	return (word << shift) | (word >> (32 - shift));
}

static u32 g339_xfs_da_hashname(const u8 *name, int namelen)
{
	u32 hash;

	for (hash = 0; namelen >= 4; namelen -= 4, name += 4)
		hash = (name[0] << 21) ^ (name[1] << 14) ^ (name[2] << 7) ^
		       (name[3] << 0) ^ g339_rol32(hash, 7 * 4);
	return hash;
}

static u8 g339_gen_rand(void)
{
	u8 r;

	while (!(r = get_random_u8()))
		;
	return r;
}

static void g339_gen_name(u8 *buffer)
{
	u32 hash, last;
	int idx;

again:
	for (idx = 0; idx < G339_NAMELEN - 4; idx++)
		buffer[idx] = g339_gen_rand();
	hash = g339_rol32(g339_xfs_da_hashname(buffer, 248), 7 * 4);
	last = hash ^ ~0U;
	if (last == 0)
		goto again;
	buffer[idx + 3] = last & 0x7fU;
	buffer[idx + 2] = (last >> 7) & 0x7fU;
	buffer[idx + 1] = (last >> 14) & 0x7fU;
	buffer[idx + 0] = (last >> 21) & 0xffU;
	/* a computed byte of 0 ends the name early, as it does upstream */
	buffer[G339_NAMELEN] = '\0';
	if (memchr(buffer, '.', G339_NAMELEN) ||
	    memchr(buffer, '/', G339_NAMELEN))
		goto again;
}

struct g339_walk {
	struct dir_context	ctx;
	char			*names;	/* G339_N + 1 slots */
	int			count;
	int			overflow;
	int			added;
};

static bool g339_actor(struct dir_context *ctx, const char *name, int len,
		       loff_t off, u64 ino, unsigned int type)
{
	struct g339_walk *w = container_of(ctx, struct g339_walk, ctx);

	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	w->added++;
	if (w->count > G339_N || len > G339_NAMELEN) {
		w->overflow++;
		return true;
	}
	memcpy(w->names + w->count * (G339_NAMELEN + 1), name, len);
	w->names[w->count * (G339_NAMELEN + 1) + len] = '\0';
	w->count++;
	return true;
}

static void g339_kvfree(void *p)
{
	kvfree(p);
}

static void g339_remove_tree(void *unused)
{
	xfs_scratch_umount();
}

static void many_long_binary_names_survive_a_mount_cycle(struct kunit *test)
{
	struct g339_walk w = { .ctx.actor = g339_actor };
	char *path, *name;
	int i, err, created = 0;
	struct file *d;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g339_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G339_DIR), 0);

	path = kunit_kmalloc(test, PATH_MAX, GFP_KERNEL);
	name = kunit_kmalloc(test, G339_NAMELEN + 1, GFP_KERNEL);
	w.names = kvmalloc_array(G339_N + 1, G339_NAMELEN + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w.names);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g339_kvfree, w.names),
			0);

	/* dirhash_collide -d -n 10000 */
	for (i = 0; i < G339_N; i++) {
		g339_gen_name(name);
		snprintf(path, PATH_MAX, G339_DIR "/%s", name);
		err = xfs_mkdir(path);
		if (!err)
			created++;
		else if (err != -EEXIST)
			KUNIT_FAIL_AND_ABORT(test, "mkdir %d: %d", i, err);
	}

	/* _scratch_unmount; _scratch_mount */
	KUNIT_ASSERT_EQ(test, xfs_scratch_umount(), 0);
	KUNIT_ASSERT_EQ(test, xfs_scratch_mount(), 0);

	/* rm -rf: read the whole directory, then remove every entry */
	d = filp_open(G339_DIR, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(d), "opendir: %ld", PTR_ERR(d));
	do {
		w.added = 0;
		err = iterate_dir(d, &w.ctx);
	} while (!err && w.added);
	filp_close(d, NULL);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "readdir: %d", err);
	KUNIT_EXPECT_EQ(test, w.overflow, 0);
	KUNIT_EXPECT_EQ_MSG(test, w.count, created,
			    "READDIR returned %d entries, %d were created",
			    w.count, created);

	for (i = 0; i < w.count; i++) {
		snprintf(path, PATH_MAX, G339_DIR "/%s",
			 w.names + i * (G339_NAMELEN + 1));
		err = xfs_rmdir(path);
		if (err) {
			KUNIT_FAIL(test, "rmdir of entry %d: %d", i, err);
			break;
		}
	}
	KUNIT_EXPECT_EQ(test, xfs_rmdir_settled(G339_DIR), 0);
}

static int g339_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g339_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g339_cases[] = {
	KUNIT_CASE_SLOW(many_long_binary_names_survive_a_mount_cycle),
	{}
};

static struct kunit_suite g339_suite = {
	.name		= "xfstests/generic/339",
	.suite_init	= g339_suite_init,
	.suite_exit	= g339_suite_exit,
	.test_cases	= g339_cases,
};

kunit_test_suites(&g339_suite);

MODULE_DESCRIPTION("xfstests generic/339 over a loopback NFS mount");
MODULE_LICENSE("GPL");
