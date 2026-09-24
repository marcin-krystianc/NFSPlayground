// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/412 over a loopback NFS mount: truncate into a hole
 * between a buffered and a direct write.
 *
 * Upstream writes 32k of 0x01 buffered at offset 0, 32k of 0x02 with
 * O_DIRECT at offset 64k (leaving a 32k hole between them), truncates the
 * file to 60k -- a size inside the hole, below the direct write -- and
 * compares md5sums before and after cycling the mount. A filesystem that
 * mishandles the truncate loses data or reports the wrong size after the
 * remount.
 *
 * Over NFS the mix is what makes it interesting: the buffered write is
 * still in the page cache, the direct write went straight to the server
 * (invalidating the client's pages for its range), and the truncate is a
 * SETATTR that has to reconcile both. The file must end up 60k long,
 * 0x01 for the first 32k and zeroes to the end, with the direct write
 * entirely gone -- on the client and on the server.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "xfstests_nfs_fixture.h"

#define G412_ROOT	XFS_MNT "/g412"
#define G412_FILE	G412_ROOT "/foo"
#define G412_SERVER	XFS_EXPORT "/g412/foo"

#define G412_CHUNK	(32 * 1024)
#define G412_DIO_OFF	(64 * 1024)
#define G412_TRUNC	(60 * 1024)

static void g412_remove_tree(void *unused)
{
	xfs_unlink(G412_FILE);
	xfs_rmdir_settled(G412_ROOT);
}

static void g412_verify(struct kunit *test, const char *path, u8 *buf,
			const char *which)
{
	struct kstat st;
	ssize_t n;
	loff_t i;

	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)G412_TRUNC,
			    "%s: size is %lld, expected %d", which, st.size,
			    G412_TRUNC);

	memset(buf, 0xff, G412_TRUNC);
	n = xfs_read_range(path, buf, G412_TRUNC, 0);
	KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)G412_TRUNC,
			    "%s: read returned %zd", which, n);
	for (i = 0; i < G412_TRUNC; i++) {
		u8 want = i < G412_CHUNK ? 0x01 : 0x00;

		if (buf[i] != want) {
			KUNIT_FAIL(test,
				   "%s: byte %lld is %02x, expected %02x",
				   which, i, buf[i], want);
			return;
		}
	}
}

static void a_truncate_into_a_hole_keeps_the_file_consistent(struct kunit *test)
{
	struct file *f;
	loff_t pos;
	u8 *buf;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G412_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g412_remove_tree, NULL),
			0);

	/*
	 * Sized for g412_verify()'s readback, not the write chunk below:
	 * it reads/memsets G412_TRUNC (60k) bytes through this same
	 * buffer, which is larger than G412_CHUNK (32k). A buffer sized to
	 * G412_CHUNK here overflows by 28k on every call to g412_verify().
	 */
	buf = kunit_kmalloc(test, G412_TRUNC, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* buffered 32k of 0x01 at 0 */
	memset(buf, 0x01, G412_CHUNK);
	f = filp_open(G412_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open: %ld", PTR_ERR(f));
	pos = 0;
	KUNIT_ASSERT_EQ(test, kernel_write(f, buf, G412_CHUNK, &pos),
			(ssize_t)G412_CHUNK);
	filp_close(f, NULL);

	/* direct 32k of 0x02 at 64k, leaving a hole between them */
	memset(buf, 0x02, G412_CHUNK);
	f = filp_open(G412_FILE, O_RDWR | O_DIRECT, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "direct open: %ld",
			       PTR_ERR(f));
	pos = G412_DIO_OFF;
	KUNIT_ASSERT_EQ(test, xfs_direct_write(f, buf, G412_CHUNK, &pos),
			(ssize_t)G412_CHUNK);
	filp_close(f, NULL);

	/* and truncate back into the hole */
	KUNIT_ASSERT_EQ(test, xfs_truncate(G412_FILE, G412_TRUNC), 0);

	g412_verify(test, G412_FILE, buf, "before");
	g412_verify(test, G412_SERVER, buf, "server");
}

static int g412_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g412_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g412_cases[] = {
	KUNIT_CASE(a_truncate_into_a_hole_keeps_the_file_consistent),
	{}
};

static struct kunit_suite g412_suite = {
	.name		= "xfstests/generic/412",
	.suite_init	= g412_suite_init,
	.suite_exit	= g412_suite_exit,
	.test_cases	= g412_cases,
};

kunit_test_suites(&g412_suite);

MODULE_DESCRIPTION("xfstests generic/412 over a loopback NFS mount");
MODULE_LICENSE("GPL");
