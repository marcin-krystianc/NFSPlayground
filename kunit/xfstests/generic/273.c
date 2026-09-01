// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/273 over a loopback NFS mount: copies under space pressure.
 *
 * Upstream runs a heavy parallel cp workload to shake out reservation
 * bugs: copies racing space pressure must either succeed byte-identical
 * or fail cleanly with ENOSPC -- never truncate or corrupt silently. The
 * single-threaded port: one 1 MB source, sixty copies into a 16 MB
 * export; every copy that reports success must verify, every failure
 * must be ENOSPC, and at least one of each must occur.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G273_ROOT	XFS_MNT "/g273"

#define G273_SRCSZ	(1024 * 1024)
#define G273_COPIES	60

static void g273_remove_tree(void *unused)
{
	char buf[64];
	int i;

	for (i = 0; i < G273_COPIES; i++) {
		snprintf(buf, sizeof(buf), G273_ROOT "/c%02d", i);
		xfs_unlink(buf);
	}
	xfs_unlink(G273_ROOT "/src");
	xfs_rmdir_settled(G273_ROOT);
}

/* copy via 64K chunks + final fsync; report the first error */
static int g273_copy(struct kunit *test, const char *src, const char *dst)
{
	struct file *in, *out;
	loff_t rpos = 0, wpos = 0;
	u8 *buf;
	ssize_t got;
	int err = 0;

	buf = kunit_kmalloc(test, 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	in = filp_open(src, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	out = filp_open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (IS_ERR(out)) {
		filp_close(in, NULL);
		return PTR_ERR(out);
	}
	while ((got = kernel_read(in, buf, 65536, &rpos)) > 0) {
		ssize_t w = kernel_write(out, buf, got, &wpos);

		if (w != got) {
			err = (w < 0) ? w : -EIO;
			break;
		}
	}
	if (!err && got < 0)
		err = got;
	if (!err)
		err = vfs_fsync(out, 0);
	filp_close(in, NULL);
	filp_close(out, NULL);
	return err;
}

static void copies_succeed_exactly_or_fail_enospc(struct kunit *test)
{
	char name[64];
	u8 *src, *back;
	int i, j, ok = 0, full = 0;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G273_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g273_remove_tree, NULL),
			0);

	src = kunit_kmalloc(test, G273_SRCSZ, GFP_KERNEL);
	back = kunit_kmalloc(test, G273_SRCSZ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, back);
	for (i = 0; i < G273_SRCSZ; i++)
		src[i] = (u8)(i * 31 + (i >> 12));
	KUNIT_ASSERT_EQ(test,
			xfs_write_new_file(G273_ROOT "/src", src, G273_SRCSZ), 0);

	for (i = 0; i < G273_COPIES; i++) {
		int err;

		snprintf(name, sizeof(name), G273_ROOT "/c%02d", i);
		err = g273_copy(test, G273_ROOT "/src", name);
		if (err) {
			KUNIT_ASSERT_EQ_MSG(test, err, -ENOSPC,
					    "copy %d failed with %d, not ENOSPC",
					    i, err);
			full++;
			xfs_unlink(name);	/* partial copies don't count */
			continue;
		}
		ok++;
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(name, back, G273_SRCSZ, 0),
				(ssize_t)G273_SRCSZ);
		for (j = 0; j < G273_SRCSZ; j++)
			if (back[j] != src[j]) {
				KUNIT_FAIL(test,
					   "successful copy %d corrupt at %d",
					   i, j);
				return;
			}
	}
	KUNIT_EXPECT_GT_MSG(test, ok, 5, "only %d copies fit", ok);
	KUNIT_EXPECT_GT_MSG(test, full, 0,
			    "the export never filled -- pressure never happened");
}

static int g273_suite_init(struct kunit_suite *suite)
{
	xfstests_nfs_export_opts("size=16777216,nr_inodes=8192");
	return xfstests_nfs_get();
}

static void g273_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g273_cases[] = {
	KUNIT_CASE_SLOW(copies_succeed_exactly_or_fail_enospc),
	{}
};

static struct kunit_suite g273_suite = {
	.name		= "xfstests/generic/273",
	.suite_init	= g273_suite_init,
	.suite_exit	= g273_suite_exit,
	.test_cases	= g273_cases,
};

kunit_test_suites(&g273_suite);

MODULE_DESCRIPTION("xfstests generic/273 over a loopback NFS mount");
MODULE_LICENSE("GPL");
