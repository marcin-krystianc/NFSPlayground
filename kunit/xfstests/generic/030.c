// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/030 over a loopback NFS mount: mapped writes against
 * remap plus truncate.
 *
 * generic/029's sibling. Same idea -- mapped writes interleaved with
 * truncates -- but the boundary is deliberately awkward: the file is 5017k,
 * which is 1254.25 pages, so every mapped write lands inside the partial
 * last page rather than on a page boundary. Both scenarios grow the file,
 * write through the mapping into the new tail, shrink the file again, then
 * grow it and write again. The first mapped write must be discarded by the
 * truncate down; the second must survive.
 *
 * The distinctive thing about 030 is not mremap. Upstream resizes the
 * mapping with "mremap -m 5020k" and "mremap 5017k" around the truncates,
 * and that looks like the feature under test -- it is why this test was
 * previously recorded here as unportable, since mremap has no kernel-callable
 * wrapper the way vm_mmap() does. Working out how to call it showed the
 * calls do nothing: mremap rounds both lengths up to a page, and 5017k is
 * 1254.25 pages, so PAGE_ALIGN(5017k) == PAGE_ALIGN(5020k) == 5020k. Every
 * mremap in upstream 030 hits mremap's "old_len == new_len" path and returns
 * the same address without touching a VMA. What they actually resize is
 * xfs_io's own record of the mapping length, which is what lets its next
 * mwrite pass its own bounds check.
 *
 * That was established the hard way. A wrapper was added to mm/mremap.c, the
 * grow and shrink were performed, and an assertion that a write past the
 * shrunk mapping must now fail was added to prove the shrink had taken
 * effect. It did not fail -- the tail was still mapped. The wrapper and the
 * kernel edit were then removed, since a kernel change to call a function
 * that provably does nothing is worse than no test at all. The rounding is
 * asserted directly below instead, so the finding is checked rather than
 * merely claimed in a comment.
 *
 * What is left is a distinct layout, not a distinct mechanism, and it is
 * worth being precise about how much that buys. Every mapped write here
 * lands at an unaligned offset inside the file's last page, on a ~5 MB file
 * rather than 029's 5 KB ones. But on the two mutations tried against both:
 *
 *   - dropping truncate_pagecache() in nfs_vmtruncate() (fs/nfs/inode.c:811)
 *     is caught by 029 and by 030, the latter at the mid-test check added
 *     below rather than at the final comparison.
 *   - dropping nfs_folio_length()'s partial-last-folio clamp
 *     (fs/nfs/internal.h) is caught by 029 -- whose third case is 5121
 *     bytes -- and is invisible to 030, including to the mid-test check.
 *     I predicted twice that 030 would catch this and was wrong both times;
 *     rather than guess at a third mechanism, the measurement is recorded
 *     as it stands.
 *
 * So 030 is not a strictly stronger 029. It is a second layout over the
 * same code path, which is what upstream intends it as.
 *
 * mmap from a KUnit case comes from kunit_vm_mmap() (lib/kunit/user_alloc.c),
 * which allocates an mm, runs arch_pick_mmap_layout() and attaches it with
 * kthread_use_mm(); see generic/029. Writes go through copy_to_user(), the
 * correct way to touch user addresses with a borrowed mm.
 *
 * Deviations: upstream's per-scenario "cycle the mount" cold re-read is
 * done by reading through the tmpfs export instead (the server's own
 * bytes), and the golden hexdumps become range assertions -- the file is
 * ~5 MB, so materialising a byte-exact model would exceed KMALLOC_MAX_SIZE
 * and the hexdumps encode nothing more than "this range holds this byte".
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G030_ROOT	XFS_MNT "/g030"
#define G030_FILE	G030_ROOT "/testfile"
#define G030_SERVER	XFS_EXPORT "/g030/testfile"

#define G030_SMALL	(5017 * 1024)	/* 1254.25 pages: partial last page */
#define G030_LARGE	(5020 * 1024)	/* exactly 1255 pages */
#define G030_TAIL	(G030_LARGE - G030_SMALL)	/* 3k */
#define G030_ZOFF	(5016 * 1024)	/* start of that partial page */
#define G030_ZLEN	(1 * 1024)

#define G030_X		0x58
#define G030_W		0x57
#define G030_Z		0x5a
#define G030_Y		0x59

#define G030_CHUNK	(64 * 1024)

static void g030_remove_tree(void *unused)
{
	xfs_unlink(G030_FILE);
	xfs_rmdir(G030_ROOT);
}

/* mwrite: fill a range of the mapping through copy_to_user() */
static void g030_mwrite(struct kunit *test, unsigned long addr, u8 *scratch,
			loff_t off, loff_t len, u8 val, const char *ctx)
{
	loff_t done = 0;

	memset(scratch, val, min_t(loff_t, len, G030_CHUNK));
	while (done < len) {
		size_t n = min_t(loff_t, len - done, G030_CHUNK);
		unsigned long left;

		left = copy_to_user((void __user *)(addr + off + done),
				    scratch, n);
		KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
				    "%s: mapped write at %lld left %lu bytes",
				    ctx, off + done, left);
		done += n;
	}
}

/*
 * The hexdump's meaning, as a range assertion: every byte of [off, off+len)
 * in @path must equal @val.
 */
static void g030_expect_fill(struct kunit *test, const char *path, loff_t off,
			     loff_t len, u8 val, const char *ctx,
			     const char *which)
{
	u8 *got;
	loff_t done = 0;

	got = kunit_kmalloc(test, G030_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	while (done < len) {
		size_t n = min_t(loff_t, len - done, G030_CHUNK);
		ssize_t r = xfs_read_range(path, got, n, off + done);
		size_t i;

		KUNIT_ASSERT_EQ_MSG(test, r, (ssize_t)n,
				    "%s: %s read at %lld returned %zd", ctx,
				    which, off + done, r);
		for (i = 0; i < n; i++)
			if (got[i] != val) {
				KUNIT_FAIL(test,
					   "%s: %s byte %lld is %02x, expected %02x",
					   ctx, which, off + done + (loff_t)i,
					   got[i], val);
				return;
			}
		done += n;
	}
}

/* @zlen != 0 adds upstream's second-scenario write while the file is short */
static void g030_scenario(struct kunit *test, loff_t zlen, const char *ctx)
{
	struct kstat st;
	struct file *f;
	unsigned long addr;
	u8 *scratch;
	loff_t pos;

	scratch = kunit_kmalloc(test, G030_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, scratch);

	xfs_unlink(G030_FILE);
	f = filp_open(G030_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", ctx,
			       PTR_ERR(f));

	/* truncate 5017k; pwrite X 0 5017k */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G030_FILE, G030_SMALL), 0);
	memset(scratch, G030_X, G030_CHUNK);
	for (pos = 0; pos < G030_SMALL; ) {
		size_t n = min_t(loff_t, G030_SMALL - pos, G030_CHUNK);
		loff_t p = pos;

		KUNIT_ASSERT_EQ(test, kernel_write(f, scratch, n, &p),
				(ssize_t)n);
		pos += n;
	}

	/* mmap -rw 0 5017k */
	addr = kunit_vm_mmap(test, f, 0, G030_SMALL, PROT_READ | PROT_WRITE,
			     MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, addr, 0UL, "%s: kunit_vm_mmap failed", ctx);

	/*
	 * Upstream's "mremap -m 5020k" here, checked rather than performed.
	 * A 5017k mapping is already 5020k of pages, so the whole tail this
	 * test writes into is mapped without any mremap at all -- which is
	 * why upstream's mremap calls are no-ops. If this write ever starts
	 * failing, the premise above is wrong and the mremap calls need to be
	 * ported for real.
	 */
	KUNIT_ASSERT_EQ_MSG(test, PAGE_ALIGN((unsigned long)G030_SMALL),
			    (unsigned long)G030_LARGE,
			    "%s: 5017k no longer rounds up to 5020k", ctx);
	KUNIT_ASSERT_EQ_MSG(test,
			    copy_to_user((void __user *)(addr + G030_LARGE - 1),
					 scratch, 1), 0UL,
			    "%s: the rounded-up tail of the mapping is not writable",
			    ctx);

	/* truncate up, then write W through the mapped tail */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G030_FILE, G030_LARGE), 0);
	g030_mwrite(test, addr, scratch, G030_SMALL, G030_TAIL, G030_W, ctx);

	/* truncate down: W must be discarded */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G030_FILE, G030_SMALL), 0);

	/*
	 * Upstream's second scenario writes through the mapping while the
	 * file is short, into the partial page that the truncate just
	 * trimmed. This one must survive.
	 */
	if (zlen)
		g030_mwrite(test, addr, scratch, G030_ZOFF, zlen, G030_Z, ctx);

	/* grow again and write Y into the re-extended tail */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G030_FILE, G030_LARGE), 0);

	/*
	 * Upstream never looks here: it dumps the file only at the end, by
	 * which point the Y write below has overwritten this whole range.
	 * Checking it now pins that a truncate up exposes zeros rather than
	 * the W bytes the mapping still holds for these pages, and that the Z
	 * write did not bleed past the partial i_size it was made under.
	 *
	 * This is the assertion with the teeth. Commenting out
	 * truncate_pagecache() in nfs_vmtruncate() (fs/nfs/inode.c:811) fails
	 * exactly here, on both scenarios -- "byte 5137408 is 57, expected
	 * 00", the stale W surviving the truncate down. The end-of-test
	 * comparison that upstream relies on does not notice.
	 */
	g030_expect_fill(test, G030_FILE, G030_SMALL, G030_TAIL, 0, ctx,
			 "client mid-test hole");
	if (zlen)
		g030_expect_fill(test, G030_FILE, G030_ZOFF, zlen, G030_Z, ctx,
				 "client mid-test Z");

	g030_mwrite(test, addr, scratch, G030_SMALL, G030_TAIL, G030_Y, ctx);

	/* the close is what flushes the mapped pages to the server */
	KUNIT_EXPECT_EQ_MSG(test, vm_munmap(addr, G030_LARGE), 0,
			    "%s: munmap", ctx);
	filp_close(f, NULL);

	KUNIT_ASSERT_EQ(test, xfs_kstat(G030_FILE, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G030_LARGE,
			    "%s: final size is %lld, expected %d", ctx,
			    st.size, G030_LARGE);

	/* the golden hexdump, as ranges -- server side first (post-remount) */
	g030_expect_fill(test, G030_SERVER, 0,
			 zlen ? G030_ZOFF : G030_SMALL, G030_X, ctx, "SERVER");
	if (zlen)
		g030_expect_fill(test, G030_SERVER, G030_ZOFF, zlen, G030_Z,
				 ctx, "SERVER");
	g030_expect_fill(test, G030_SERVER, G030_SMALL, G030_TAIL, G030_Y,
			 ctx, "SERVER");

	/* ...and the client agrees */
	g030_expect_fill(test, G030_FILE, G030_SMALL, G030_TAIL, G030_Y, ctx,
			 "client");
	if (zlen)
		g030_expect_fill(test, G030_FILE, G030_ZOFF, zlen, G030_Z, ctx,
				 "client");
}

static void unaligned_mapped_writes_survive_truncate_down_and_up(struct kunit *test)
{
	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G030_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g030_remove_tree,
						  NULL), 0);

	g030_scenario(test, 0, "1. no write while short");
	g030_scenario(test, G030_ZLEN, "2. write while short");
}

static int g030_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g030_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g030_cases[] = {
	KUNIT_CASE_SLOW(unaligned_mapped_writes_survive_truncate_down_and_up),
	{}
};

static struct kunit_suite g030_suite = {
	.name		= "xfstests/generic/030",
	.suite_init	= g030_suite_init,
	.suite_exit	= g030_suite_exit,
	.test_cases	= g030_cases,
};

kunit_test_suites(&g030_suite);

MODULE_DESCRIPTION("xfstests generic/030 over a loopback NFS mount");
MODULE_LICENSE("GPL");
