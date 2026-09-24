// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/135 over a loopback NFS mount: buffered, O_SYNC and
 * O_DIRECT writes all survive a cache-cold re-read.
 *
 * Upstream writes four files on the scratch filesystem -- 4k of 0x12
 * buffered, 4k of 0x34 with O_SYNC, 4k of 0x56 with O_DIRECT, and a fourth
 * written 4k of 0x78 then truncated to 2k -- cycles the mount, and dumps
 * all four with od. The golden output is the whole assertion: each file
 * has exactly the size and bytes it was left with.
 *
 * Over NFS the three write paths are genuinely different: buffered goes
 * through nfs_write_end() and writeback, O_SYNC forces the WRITE to be
 * FILE_SYNC (or a COMMIT to follow), and O_DIRECT bypasses the page cache
 * entirely via nfs_direct_write(). The truncate case additionally leaves a
 * partially-written page behind a SETATTR.
 *
 * Deviations: upstream's fourth file has a third write, "pwrite 1k 0 1k",
 * which xfs_io rejects (three positional arguments where it takes two) --
 * its golden output records 2048 bytes of 0x78, i.e. the no-op. The port
 * keeps the no-op and does not invent the write upstream does not perform.
 * The mount cycle is replaced by the two checks this fixture uses in place
 * of it: the server's own bytes through the tmpfs export, and a client
 * re-read with the page cache dropped first. The O_DIRECT write goes
 * through xfs_direct_write() rather than kernel_write(), which cannot do
 * direct I/O over NFS at all -- see nfs_fixture.c.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include "xfstests_nfs_fixture.h"

#define G135_ROOT	XFS_MNT "/g135"
#define G135_SERVER	XFS_EXPORT "/g135"
#define G135_LEN	4096

static const struct g135_file {
	const char	*name;
	int		flags;		/* extra open flags */
	bool		direct;		/* write through the O_DIRECT path */
	u8		val;
	loff_t		truncate_to;	/* 0 = no truncate */
	loff_t		expect;
} g135_files[] = {
	{ "async_file",  0,		false,	0x12, 0,    4096 },
	{ "sync_file",   O_SYNC,	false,	0x34, 0,    4096 },
	{ "direct_file", O_DIRECT,	true,	0x56, 0,    4096 },
	{ "trunc_file",  0,		false,	0x78, 2048, 2048 },
};

static void g135_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g135_files); i++) {
		snprintf(path, sizeof(path), G135_ROOT "/%s",
			 g135_files[i].name);
		xfs_unlink(path);
	}
	xfs_rmdir_settled(G135_ROOT);
}

static void g135_check(struct kunit *test, const char *path,
		       const struct g135_file *fi, const char *which)
{
	struct kstat st;
	u8 *buf;
	loff_t i;
	ssize_t n;

	if (!strcmp(which, "client")) {
		KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
		KUNIT_EXPECT_EQ_MSG(test, st.size, fi->expect,
				    "%s: size is %lld, expected %lld",
				    fi->name, st.size, fi->expect);
	}

	buf = kunit_kmalloc(test, G135_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0, G135_LEN);

	/* one byte past the expected end: it must not be readable */
	n = xfs_read_range(path, buf, fi->expect + 1, 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)fi->expect,
			    "%s: %s read returned %zd, expected %lld",
			    fi->name, which, n, fi->expect);
	for (i = 0; i < fi->expect; i++)
		if (buf[i] != fi->val) {
			KUNIT_FAIL(test, "%s: %s byte %lld is %02x, expected %02x",
				   fi->name, which, i, buf[i], fi->val);
			return;
		}
}

static void every_write_path_lands_the_same_bytes(struct kunit *test)
{
	char path[64], spath[64];
	u8 *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G135_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g135_remove_tree, NULL),
			0);

	buf = kunit_kmalloc(test, G135_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < ARRAY_SIZE(g135_files); i++) {
		const struct g135_file *fi = &g135_files[i];
		struct file *f;
		loff_t pos = 0;

		snprintf(path, sizeof(path), G135_ROOT "/%s", fi->name);

		memset(buf, fi->val, G135_LEN);
		f = filp_open(path, O_RDWR | O_CREAT | O_TRUNC | fi->flags,
			      0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld",
				       fi->name, PTR_ERR(f));
		KUNIT_ASSERT_EQ_MSG(test,
				    fi->direct ?
					xfs_direct_write(f, buf, G135_LEN, &pos) :
					kernel_write(f, buf, G135_LEN, &pos),
				    (ssize_t)G135_LEN, "%s: write failed",
				    fi->name);
		filp_close(f, NULL);

		if (fi->truncate_to)
			KUNIT_ASSERT_EQ_MSG(test,
					    xfs_truncate(path, fi->truncate_to),
					    0, "%s: truncate failed",
					    fi->name);
	}

	/* upstream's _scratch_cycle_mount, in the two forms available here */
	for (i = 0; i < ARRAY_SIZE(g135_files); i++) {
		const struct g135_file *fi = &g135_files[i];
		struct file *f;

		snprintf(path, sizeof(path), G135_ROOT "/%s", fi->name);
		snprintf(spath, sizeof(spath), G135_SERVER "/%s", fi->name);

		f = filp_open(path, O_RDONLY, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f));
		KUNIT_EXPECT_EQ(test, invalidate_inode_pages2(f->f_mapping), 0);
		filp_close(f, NULL);

		g135_check(test, path, fi, "client");
		g135_check(test, spath, fi, "server");
	}
}

static int g135_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g135_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g135_cases[] = {
	KUNIT_CASE(every_write_path_lands_the_same_bytes),
	{}
};

static struct kunit_suite g135_suite = {
	.name		= "xfstests/generic/135",
	.suite_init	= g135_suite_init,
	.suite_exit	= g135_suite_exit,
	.test_cases	= g135_cases,
};

kunit_test_suites(&g135_suite);

MODULE_DESCRIPTION("xfstests generic/135 over a loopback NFS mount");
MODULE_LICENSE("GPL");
