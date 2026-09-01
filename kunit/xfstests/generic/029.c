// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/029 over a loopback NFS mount: mapped writes against
 * truncate down and up.
 *
 * Upstream drives xfs_io through three scenarios of the same shape: fill a
 * file, mmap it read-write, write through the mapping, truncate the file
 * down, truncate it back up, write through the mapping again, then hexdump
 * the result both before and after cycling the mount. It is looking for
 * data-corruption bugs where a partially-dirty page meets a truncate --
 * classically on filesystems whose block size is below the page size, but
 * the interaction is general.
 *
 * Over NFS the whole point is that a mapped write dirties a page through
 * nfs_vm_page_mkwrite() rather than through nfs_write_end(), and then
 * nfs_setattr()'s truncate has to reconcile that with truncate_pagecache()
 * and writeback. Nothing else in the ported set reaches the mmap path.
 *
 * I previously recorded this test as impossible to port, on the grounds
 * that vm_mmap() needs current->mm and a KUnit case runs in a kernel
 * thread which has none. That was wrong: KUnit ships kunit_vm_mmap()
 * (lib/kunit/user_alloc.c), which allocates an mm, runs
 * arch_pick_mmap_layout() on it, attaches it with kthread_use_mm() and
 * tracks the mapping as a test resource. mm_alloc() is even already
 * EXPORT_SYMBOL_IF_KUNIT for the purpose. So mapped writes are available
 * to every port, and the "no mmap from a kthread" note in the docs was
 * simply a failure to look.
 *
 * Writes into the mapping go through copy_to_user(), which is the correct
 * way to touch user addresses from kernel context with a borrowed mm; a
 * bare dereference would work on UML but not on architectures with SMAP
 * or PAN.
 *
 * Deviations: upstream's per-scenario "cycle the mount" cold re-read is
 * done here by reading the file through the tmpfs export instead -- the
 * server's own bytes, which is a stronger check than a remounted client
 * and does not disturb the shared fixture. The golden hexdumps are
 * replaced by a byte-exact model built the same way the file is.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "xfstests_nfs_fixture.h"

#define G029_ROOT	XFS_MNT "/g029"
#define G029_FILE	G029_ROOT "/testfile"
#define G029_SERVER	XFS_EXPORT "/g029/testfile"
#define G029_MAX	8192

/*
 * The three scenarios, in upstream's order and with its byte values:
 * X fill, Z mapped write, truncate down, optional W mapped write while
 * short, truncate up, Y mapped write.
 */
static const struct g029_case {
	const char	*name;
	loff_t		size;		/* initial truncate + pwrite length */
	loff_t		zoff, zlen;	/* mapped write before truncating */
	loff_t		tdown;		/* truncate down to */
	loff_t		woff, wlen;	/* mapped write while short; 0 = none */
	loff_t		tup;		/* truncate back up to */
	loff_t		yoff, ylen;	/* mapped write after truncating up */
} g029_cases[] = {
	{
		"1. aligned, no write while short",
		5120,  2048, 3072,  2048,  0, 0,        5120,  2048, 3072,
	},
	{
		"2. aligned, write while short",
		5120,  2048, 3072,  2048,  1024, 1024,  5120,  2048, 3072,
	},
	{
		"3. unaligned throughout",
		5121,  2047, 3071,  2047,  513, 1025,   5121,  2047, 3071,
	},
};

#define G029_X	0x58
#define G029_Z	0x5a
#define G029_W	0x57
#define G029_Y	0x59

static void g029_remove_tree(void *unused)
{
	xfs_unlink(G029_FILE);
	xfs_rmdir(G029_ROOT);
}

/* mwrite: fill a range of the mapping through copy_to_user() */
static void g029_mwrite(struct kunit *test, unsigned long addr, u8 *scratch,
			loff_t off, loff_t len, u8 val, const char *ctx)
{
	loff_t done = 0;

	memset(scratch, val, min_t(loff_t, len, PAGE_SIZE));
	while (done < len) {
		size_t n = min_t(loff_t, len - done, PAGE_SIZE);
		unsigned long left;

		left = copy_to_user((void __user *)(addr + off + done),
				    scratch, n);
		KUNIT_ASSERT_EQ_MSG(test, left, 0UL,
				    "%s: mapped write at %lld left %lu bytes unwritten",
				    ctx, off + done, left);
		done += n;
	}
}

static void g029_verify(struct kunit *test, const char *path, const u8 *want,
			loff_t size, const char *ctx, const char *which)
{
	u8 *got;
	loff_t i;
	ssize_t n;

	got = kunit_kmalloc(test, G029_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, got);

	n = xfs_read_range(path, got, size, 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)size,
			    "%s: %s read returned %zd, expected %lld", ctx,
			    which, n, size);
	for (i = 0; i < size; i++)
		if (got[i] != want[i]) {
			KUNIT_FAIL(test,
				   "%s: %s byte %lld is %02x, expected %02x",
				   ctx, which, i, got[i], want[i]);
			return;
		}
}

static void mapped_writes_survive_truncate_down_and_up(struct kunit *test)
{
	u8 *want, *scratch;
	int ci;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G029_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g029_remove_tree,
						  NULL), 0);

	want = kunit_kmalloc(test, G029_MAX, GFP_KERNEL);
	scratch = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, want);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, scratch);

	for (ci = 0; ci < ARRAY_SIZE(g029_cases); ci++) {
		const struct g029_case *c = &g029_cases[ci];
		struct kstat st;
		struct file *f;
		unsigned long addr;
		loff_t pos;

		xfs_unlink(G029_FILE);

		f = filp_open(G029_FILE, O_RDWR | O_CREAT | O_EXCL, 0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld",
				       c->name, PTR_ERR(f));

		/* truncate, then pwrite the X fill -- ordinary writes */
		KUNIT_ASSERT_EQ(test, xfs_truncate(G029_FILE, c->size), 0);
		memset(scratch, G029_X, PAGE_SIZE);
		for (pos = 0; pos < c->size; ) {
			size_t n = min_t(loff_t, c->size - pos, PAGE_SIZE);
			loff_t p = pos;

			KUNIT_ASSERT_EQ(test, kernel_write(f, scratch, n, &p),
					(ssize_t)n);
			pos += n;
		}
		memset(want, G029_X, c->size);

		/* mmap -rw 0 size */
		addr = kunit_vm_mmap(test, f, 0, c->size,
				     PROT_READ | PROT_WRITE, MAP_SHARED, 0);
		KUNIT_ASSERT_NE_MSG(test, addr, 0UL,
				    "%s: kunit_vm_mmap failed", c->name);

		/* mapped write, then throw it away with a truncate down */
		g029_mwrite(test, addr, scratch, c->zoff, c->zlen, G029_Z,
			    c->name);
		memset(want + c->zoff, G029_Z, c->zlen);

		KUNIT_ASSERT_EQ_MSG(test, xfs_truncate(G029_FILE, c->tdown), 0,
				    "%s: truncate down", c->name);
		/* everything at or past the new size is gone */

		/* optionally write through the mapping while the file is short */
		if (c->wlen) {
			g029_mwrite(test, addr, scratch, c->woff, c->wlen,
				    G029_W, c->name);
			memset(want + c->woff, G029_W, c->wlen);
		}

		KUNIT_ASSERT_EQ_MSG(test, xfs_truncate(G029_FILE, c->tup), 0,
				    "%s: truncate up", c->name);
		/* the re-extended region reads as a hole */
		memset(want + c->tdown, 0, c->tup - c->tdown);

		/* and one more mapped write into the re-extended range */
		g029_mwrite(test, addr, scratch, c->yoff, c->ylen, G029_Y,
			    c->name);
		memset(want + c->yoff, G029_Y, c->ylen);

		/*
		 * Unmap and close. The close is what flushes the mapped
		 * dirty pages to the server (nfs4_file_flush -> nfs_wb_all),
		 * which is what makes the server-side comparison below the
		 * equivalent of upstream's post-remount hexdump.
		 */
		KUNIT_EXPECT_EQ_MSG(test, vm_munmap(addr, c->size), 0,
				    "%s: munmap", c->name);
		filp_close(f, NULL);

		KUNIT_ASSERT_EQ(test, xfs_kstat(G029_FILE, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, c->tup,
				    "%s: final size is %lld, expected %lld",
				    c->name, st.size, c->tup);

		/* upstream's post-remount check: the server's own bytes */
		g029_verify(test, G029_SERVER, want, c->tup, c->name,
			    "SERVER");
		/* and the client agrees */
		g029_verify(test, G029_FILE, want, c->tup, c->name, "client");
	}
}

static int g029_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g029_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g029_cases_tab[] = {
	KUNIT_CASE(mapped_writes_survive_truncate_down_and_up),
	{}
};

static struct kunit_suite g029_suite = {
	.name		= "xfstests/generic/029",
	.suite_init	= g029_suite_init,
	.suite_exit	= g029_suite_exit,
	.test_cases	= g029_cases_tab,
};

kunit_test_suites(&g029_suite);

MODULE_DESCRIPTION("xfstests generic/029 over a loopback NFS mount");
MODULE_LICENSE("GPL");
