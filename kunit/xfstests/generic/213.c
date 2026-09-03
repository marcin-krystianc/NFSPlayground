// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/213 over a loopback NFS mount: fallocate against truncate.
 *
 * Upstream's five scenarios, each on a fresh file (tests/generic/213):
 *
 *	falloc 0 1g			truncate 100	 (truncate far below)
 *	falloc 0 1g			truncate 1g	 (truncate to the end)
 *	falloc 0 1g			truncate 2g	 (truncate past it)
 *	falloc 0 1g, falloc 2g 1m	truncate 3g	 (two ranges, a hole)
 *	falloc 0 $((avail*2))k				 (must be ENOSPC)
 *
 * Over NFSv4.2 each falloc is an ALLOCATE and each truncate a SETATTR, so
 * what is being checked is that the two agree about the size afterwards and
 * that an allocation the server cannot satisfy comes back as ENOSPC rather
 * than as a short or silent success.
 *
 * The magnitudes cannot be kept: the export is a 64 MB tmpfs inside the test
 * kernel, and unlike a disk filesystem a 1 GiB tmpfs allocation really does
 * claim 1 GiB of memory. Each scenario is therefore scaled by the same
 * factor (1 GiB -> 8 MiB), which preserves what each one is: a truncate
 * below, at, and beyond the allocated range, and a second range past a hole.
 * The last scenario needs no scaling -- like upstream it asks for twice
 * whatever statfs reports free.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/falloc.h>
#include <linux/statfs.h>

#include "xfstests_nfs_fixture.h"

#define G213_ROOT	XFS_MNT "/g213"
#define G213_FILE	G213_ROOT "/ouch"

/* upstream's 1g, scaled to what an in-kernel tmpfs export can hold */
#define G213_UNIT	(8 * 1024 * 1024)

static void g213_remove_tree(void *unused)
{
	xfs_unlink(G213_FILE);
	xfs_rmdir(G213_ROOT);
}

/* "-c 'falloc ...' -c 'truncate ...'" on a fresh file, then the size */
static void g213_falloc_then_truncate(struct kunit *test, loff_t alloc_off,
				      loff_t alloc_len, loff_t alloc2_off,
				      loff_t alloc2_len, loff_t trunc,
				      const char *what)
{
	struct kstat st;
	struct file *f;
	int err;

	xfs_unlink(G213_FILE);
	f = filp_open(G213_FILE, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
			       PTR_ERR(f));

	err = vfs_fallocate(f, 0, alloc_off, alloc_len);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: falloc %lld %lld: %d", what,
			    alloc_off, alloc_len, err);
	if (alloc2_len) {
		err = vfs_fallocate(f, 0, alloc2_off, alloc2_len);
		KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: falloc %lld %lld: %d",
				    what, alloc2_off, alloc2_len, err);
	}

	/* the allocation itself has extended the file to cover its range */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size,
			    alloc2_len ? alloc2_off + alloc2_len
				       : alloc_off + alloc_len,
			    "%s: size after ALLOCATE is %lld", what, st.size);

	err = xfs_truncate(G213_FILE, trunc);
	KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: truncate %lld: %d", what, trunc,
			    err);
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, trunc,
			    "%s: size after truncate is %lld, expected %lld",
			    what, st.size, trunc);

	filp_close(f, NULL);
	xfs_unlink(G213_FILE);
}

static void falloc_and_truncate_agree_about_the_size(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G213_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g213_remove_tree, NULL),
			0);

	/* reserve, truncate at 100 bytes */
	g213_falloc_then_truncate(test, 0, G213_UNIT, 0, 0, 100,
				  "truncate below the allocation");
	/* reserve, truncate at the end of it */
	g213_falloc_then_truncate(test, 0, G213_UNIT, 0, 0, G213_UNIT,
				  "truncate to the allocation");
	/* reserve, truncate past it */
	g213_falloc_then_truncate(test, 0, G213_UNIT, 0, 0, 2 * G213_UNIT,
				  "truncate past the allocation");
	/* reserve, a hole, reserve again, truncate past both */
	g213_falloc_then_truncate(test, 0, G213_UNIT, 2 * G213_UNIT,
				  1024 * 1024, 3 * G213_UNIT,
				  "two allocations with a hole");
}

static void an_allocation_larger_than_the_filesystem_is_enospc(struct kunit *test)
{
	struct kstatfs sfs;
	struct kstat st;
	struct file *f;
	loff_t toobig;
	int err;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G213_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g213_remove_tree, NULL),
			0);

	KUNIT_ASSERT_EQ(test, xfs_statfs(G213_ROOT, &sfs), 0);
	KUNIT_ASSERT_GT(test, sfs.f_bavail, 0LL);
	/* upstream: let toobig=$avail*2 */
	toobig = (loff_t)sfs.f_bavail * sfs.f_bsize * 2;

	xfs_unlink(G213_FILE);
	f = filp_open(G213_FILE, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0644);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f));

	err = vfs_fallocate(f, 0, 0, toobig);
	KUNIT_EXPECT_EQ_MSG(test, err, -ENOSPC,
			    "allocating %lld bytes on a filesystem with %lld free returned %d, expected ENOSPC",
			    toobig, (loff_t)sfs.f_bavail * sfs.f_bsize, err);

	/* a refused ALLOCATE must not have left the size behind either */
	KUNIT_ASSERT_EQ(test, xfs_kstat(G213_FILE, &st), 0);
	KUNIT_EXPECT_LT_MSG(test, st.size, toobig,
			    "the refused allocation still grew the file to %lld",
			    st.size);

	filp_close(f, NULL);
	xfs_unlink(G213_FILE);
}

static int g213_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g213_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g213_cases[] = {
	KUNIT_CASE_SLOW(falloc_and_truncate_agree_about_the_size),
	KUNIT_CASE(an_allocation_larger_than_the_filesystem_is_enospc),
	{}
};

static struct kunit_suite g213_suite = {
	.name		= "xfstests/generic/213",
	.suite_init	= g213_suite_init,
	.suite_exit	= g213_suite_exit,
	.test_cases	= g213_cases,
};

kunit_test_suites(&g213_suite);

MODULE_DESCRIPTION("xfstests generic/213 over a loopback NFS mount");
MODULE_LICENSE("GPL");
