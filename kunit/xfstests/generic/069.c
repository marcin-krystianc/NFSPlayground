// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/069 over a loopback NFS mount: O_APPEND writes.
 *
 * Upstream starts six src/append_writer processes at once, with 1, 20,
 * 300, 40000, 3000000 and 12345 as their counts. Each opens its own
 * testfile.<pid> with O_CREAT | O_RDWR | O_APPEND and writes the integers
 * 0 .. count-1, four bytes at a time, through that one descriptor. When
 * all six are done, src/append_reader reads each file back and fails on
 * the first integer that is not its own index, or on a short read.
 *
 * Over NFS O_APPEND is the client's job: every write has to land at the
 * EOF the client believes in, revalidated against the server, while five
 * other writers are doing the same on the same mount. A lost or doubled
 * append shows up as an integer out of sequence.
 *
 * Each writer is a kthread with its own file, named after its index
 * rather than a pid. The port also checks each file's size, which
 * append_reader implies (it stops at EOF) but does not state.
 *
 * A second case, not in upstream, reopens the file for every append --
 * 500 appends of 1 to 2048 bytes with the size checked against the server
 * every 50 -- so each append starts from a fresh open's view of EOF.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include "xfstests_nfs_fixture.h"

#define G069_ROOT	XFS_MNT "/g069"
#define G069_CHUNK	4096
#define G069_REOPENED	G069_ROOT "/reopened"
#define G069_APPENDS	500

static const int g069_sizes[] = { 1, 20, 300, 40000, 3000000, 12345 };

struct g069_writer {
	int			n;
	int			count;
	int			err;
	struct completion	done;
};

static void g069_path(char *buf, size_t len, int n)
{
	snprintf(buf, len, G069_ROOT "/testfile.%d", n);
}

/* append_writer: count four-byte integers through one O_APPEND fd */
static int g069_append_writer(void *arg)
{
	struct g069_writer *w = arg;
	struct file *f;
	char path[64];
	int i;

	g069_path(path, sizeof(path), w->n);
	f = filp_open(path, O_CREAT | O_RDWR | O_APPEND, 0600);
	if (IS_ERR(f)) {
		w->err = PTR_ERR(f);
		goto out;
	}
	for (i = 0; i < w->count; i++) {
		loff_t pos = 0;	/* O_APPEND ignores it */
		ssize_t n = kernel_write(f, &i, sizeof(i), &pos);

		if (n != sizeof(i)) {
			w->err = n < 0 ? (int)n : -EIO;
			break;
		}
		if (!(i & 1023))
			cond_resched();
	}
	filp_close(f, NULL);
out:
	complete(&w->done);
	return 0;
}

static void g069_remove_tree(void *unused)
{
	char path[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(g069_sizes); i++) {
		g069_path(path, sizeof(path), i);
		xfs_unlink(path);
	}
	xfs_unlink(G069_REOPENED);
	xfs_rmdir_settled(G069_ROOT);
}

/* append_reader: the file is exactly 0, 1, 2, ... count-1 */
static void g069_append_reader(struct kunit *test, int n, int count, int *buf)
{
	char path[64];
	struct kstat st;
	struct file *f;
	loff_t pos = 0;
	int i = 0, j;

	g069_path(path, sizeof(path), n);
	KUNIT_ASSERT_EQ(test, xfs_kstat(path, &st), 0);
	KUNIT_EXPECT_EQ_MSG(test, st.size, (loff_t)count * sizeof(int),
			    "file with %d integers is %lld bytes", count,
			    st.size);
	f = filp_open(path, O_RDONLY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "open %s: %ld", path,
			       PTR_ERR(f));
	for (;;) {
		ssize_t got = kernel_read(f, buf, G069_CHUNK, &pos);

		if (got <= 0) {
			KUNIT_EXPECT_EQ_MSG(test, got, 0L,
					    "read error at offset %d", i * 4);
			break;
		}
		KUNIT_ASSERT_EQ_MSG(test, got % sizeof(int), 0UL,
				    "couldn't read: short read of %zd", got);
		for (j = 0; j < got / sizeof(int); j++, i++)
			if (buf[j] != i) {
				KUNIT_FAIL(test,
					   "maybe corrupt O_APPEND to testfile.%d: bad data, offset = %u, got %d wanted %d",
					   n, i * 4, buf[j], i);
				filp_close(f, NULL);
				return;
			}
	}
	filp_close(f, NULL);
	KUNIT_EXPECT_EQ(test, i, count);
}

static void concurrent_appenders_each_write_their_sequence(struct kunit *test)
{
	struct g069_writer w[ARRAY_SIZE(g069_sizes)] = {};
	struct task_struct *t;
	int *buf;
	int i;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G069_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g069_remove_tree, NULL),
			0);
	buf = kunit_kmalloc(test, G069_CHUNK, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < ARRAY_SIZE(g069_sizes); i++) {
		w[i].n = i;
		w[i].count = g069_sizes[i];
		init_completion(&w[i].done);
		t = kthread_run(g069_append_writer, &w[i], "g069-%d", i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "kthread_run: %ld",
				       PTR_ERR(t));
	}
	for (i = 0; i < ARRAY_SIZE(g069_sizes); i++)
		wait_for_completion(&w[i].done);

	for (i = 0; i < ARRAY_SIZE(g069_sizes); i++) {
		KUNIT_EXPECT_EQ_MSG(test, w[i].err, 0,
				    "the writer of %d integers failed: %d",
				    g069_sizes[i], w[i].err);
		/* "*** checking file with $size integers" */
		g069_append_reader(test, i, g069_sizes[i], buf);
	}
}

/* not in upstream: one open per append, the size checked as it grows */
static u32 g069_len(int i)
{
	/* deterministic, varied, 1..2048 */
	return (i * 2654435761u % 2048) + 1;
}

static void appends_through_fresh_opens_accumulate_exactly(struct kunit *test)
{
	struct kstat st;
	struct file *f;
	u8 *buf;
	loff_t expected = 0, pos;
	ssize_t n;
	int i;
	u32 j;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G069_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g069_remove_tree, NULL),
			0);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G069_REOPENED, "", 0), 0);

	buf = kunit_kmalloc(test, 2048, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < G069_APPENDS; i++) {
		u32 len = g069_len(i);

		f = filp_open(G069_REOPENED, O_WRONLY | O_APPEND, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f));
		for (j = 0; j < len; j++)
			buf[j] = (u8)(i ^ j);
		pos = 0;	/* O_APPEND ignores the position */
		n = kernel_write(f, buf, len, &pos);
		filp_close(f, NULL);
		KUNIT_ASSERT_EQ_MSG(test, n, (ssize_t)len,
				    "append %d short: %zd", i, n);
		expected += len;

		if (i % 50 == 49) {
			KUNIT_ASSERT_EQ(test, xfs_kstat(G069_REOPENED, &st), 0);
			KUNIT_ASSERT_EQ_MSG(test, st.size, expected,
					    "after append %d: size %lld, expected %lld",
					    i, st.size, expected);
		}
	}

	/* every appended chunk landed exactly once, in order */
	pos = 0;
	for (i = 0; i < G069_APPENDS; i++) {
		u32 len = g069_len(i);

		n = xfs_read_range(G069_REOPENED, buf, len, pos);
		KUNIT_ASSERT_EQ(test, n, (ssize_t)len);
		for (j = 0; j < len; j++)
			if (buf[j] != (u8)(i ^ j)) {
				KUNIT_FAIL(test,
					   "chunk %d corrupt at byte %u", i, j);
				return;
			}
		pos += len;
	}
}

static int g069_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g069_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g069_cases[] = {
	KUNIT_CASE_SLOW(concurrent_appenders_each_write_their_sequence),
	KUNIT_CASE_SLOW(appends_through_fresh_opens_accumulate_exactly),
	{}
};

static struct kunit_suite g069_suite = {
	.name		= "xfstests/generic/069",
	.suite_init	= g069_suite_init,
	.suite_exit	= g069_suite_exit,
	.test_cases	= g069_cases,
};

kunit_test_suites(&g069_suite);

MODULE_DESCRIPTION("xfstests generic/069 over a loopback NFS mount");
MODULE_LICENSE("GPL");
