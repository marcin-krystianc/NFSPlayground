// SPDX-License-Identifier: GPL-2.0
/*
 * xfstests generic/478 over a loopback NFS mount: OFD and POSIX lock
 * conflicts, and which closes release them.
 *
 * Upstream runs src/t_ofd_locks as a setter and a getter process on the
 * same file, 30 cases in three variants. The setter opens the file and
 * takes an OFD lock (F_OFD_SETLKW) or, with -P, a POSIX lock (F_SETLKW)
 * on [0,9]. The getter opens the file itself and asks F_OFD_GETLK or
 * F_GETLK about a range, and prints what it would conflict with:
 *
 *   plain  the setter clones a child without CLONE_FILES and the child
 *          closes its copy of the fd. Neither lock is released.
 *   -F     the child shares the fd table (CLONE_FILES) and closes the
 *          only fd. Both kinds of lock are released.
 *   -d     the setter dups the fd and closes the dup. The POSIX lock is
 *          released (any close by the owner does that), the OFD lock is
 *          not (the open file is still referenced).
 *
 * The owners are what makes these differ, and the kernel models them as
 * fl_owner values: a POSIX lock belongs to the fd table (current->files),
 * an OFD lock to the struct file. The port uses one token per fd table
 * and does each close as filp_close(file, owner), which is what close(2)
 * does. Over NFSv4 each owner becomes its own lock owner on the server,
 * a lock is LOCK, a test is a local check then LOCKT, and a release is
 * LOCKU.
 *
 * The expected results are upstream's 478.out, which matches the last
 * three arguments of each do_test line.
 *
 * Deviations: the processes and the semaphores that order them are one
 * thread doing the steps in order. The final fput of the -F close is
 * deferred for a kernel thread, so the port flushes it before the getter
 * asks.
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>

#include "xfstests_nfs_fixture.h"

#define G478_ROOT	XFS_MNT "/g478"
#define G478_FILE	G478_ROOT "/testfile"

/* one fd table each: the setter, its non-CLONE_FILES child, the getter */
static char g478_setter_files, g478_child_files, g478_getter_files;

enum { G478_PLAIN, G478_CLONE_FILES, G478_DUP, G478_VARIANTS };
static const char * const g478_variant_names[] = {
	"plain", "CLONE_FILES", "dup+close",
};

static const struct g478_case {
	unsigned char	s_type;
	bool		s_rdonly, s_posix;
	loff_t		s_start, s_len;
	unsigned char	g_type;
	bool		g_rdonly, g_posix;
	loff_t		g_start, g_len;
	unsigned char	want[G478_VARIANTS];
} g478_table[] = {
	{ F_WRLCK, false, false, 0, 10,  F_WRLCK, false, false, 0, 10,  { F_WRLCK, F_UNLCK, F_WRLCK } },
	{ F_WRLCK, false, false, 0, 10,  F_WRLCK, false, true, 5, 20,  { F_WRLCK, F_UNLCK, F_WRLCK } },
	{ F_WRLCK, false, false, 0, 10,  F_WRLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_WRLCK, false, true, 0, 10,  F_WRLCK, false, false, 5, 20,  { F_WRLCK, F_UNLCK, F_UNLCK } },
	{ F_WRLCK, false, true, 0, 10,  F_WRLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_WRLCK, false, false, 0, 10,  F_RDLCK, false, false, 0, 10,  { F_WRLCK, F_UNLCK, F_WRLCK } },
	{ F_WRLCK, false, false, 0, 10,  F_RDLCK, false, true, 5, 20,  { F_WRLCK, F_UNLCK, F_WRLCK } },
	{ F_WRLCK, false, false, 0, 10,  F_RDLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_WRLCK, false, true, 0, 10,  F_RDLCK, false, false, 5, 20,  { F_WRLCK, F_UNLCK, F_UNLCK } },
	{ F_WRLCK, false, true, 0, 10,  F_RDLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_WRLCK, true, false, 0, 10,  { F_RDLCK, F_UNLCK, F_RDLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_WRLCK, true, true, 5, 20,  { F_RDLCK, F_UNLCK, F_RDLCK } },
	{ F_RDLCK, true, true, 0, 10,  F_WRLCK, true, false, 5, 20,  { F_RDLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, false, 0, 10,  F_WRLCK, false, false, 0, 10,  { F_RDLCK, F_UNLCK, F_RDLCK } },
	{ F_RDLCK, false, false, 0, 10,  F_WRLCK, false, true, 5, 20,  { F_RDLCK, F_UNLCK, F_RDLCK } },
	{ F_RDLCK, false, true, 0, 10,  F_WRLCK, false, false, 5, 20,  { F_RDLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_WRLCK, true, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, true, 0, 10,  F_WRLCK, true, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, false, 0, 10,  F_WRLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, true, 0, 10,  F_WRLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_RDLCK, true, false, 0, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_RDLCK, true, true, 0, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, true, 0, 10,  F_RDLCK, true, false, 0, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, false, 0, 10,  F_RDLCK, false, false, 0, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, true, 0, 10,  F_RDLCK, false, false, 0, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_RDLCK, true, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, false, 0, 10,  F_RDLCK, true, true, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, true, true, 0, 10,  F_RDLCK, true, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, false, 0, 10,  F_RDLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
	{ F_RDLCK, false, true, 0, 10,  F_RDLCK, false, false, 20, 10,  { F_UNLCK, F_UNLCK, F_UNLCK } },
};

/*
 * fcntl_setlk()/fcntl_getlk() for one struct flock: OFD locks are owned
 * by the file and carry pid -1, POSIX locks by the fd table.
 */
static int g478_lock(struct file *f, bool getlk, bool posix,
		     unsigned char type, loff_t start, loff_t len,
		     fl_owner_t files, unsigned char *result)
{
	struct file_lock *fl;
	int err;

	fl = locks_alloc_lock();
	if (!fl)
		return -ENOMEM;
	fl->c.flc_type = type;
	fl->c.flc_flags = FL_POSIX | (posix ? 0 : FL_OFDLCK) |
			  (getlk ? 0 : FL_SLEEP);
	fl->c.flc_owner = posix ? files : f;
	fl->c.flc_pid = posix ? current->tgid : -1;
	fl->c.flc_file = f;
	fl->fl_start = start;
	fl->fl_end = start + len - 1;
	if (getlk) {
		err = vfs_test_lock(f, fl);
		*result = fl->c.flc_type;
	} else {
		err = vfs_lock_file(f, F_SETLKW, fl, NULL);
	}
	locks_free_lock(fl);
	return err;
}

static const char *g478_type_name(unsigned char type)
{
	return type == F_WRLCK ? "wrlck" : type == F_RDLCK ? "rdlck" : "unlck";
}

static void g478_one(struct kunit *test, int ci, int variant)
{
	const struct g478_case *c = &g478_table[ci];
	struct file *sf, *gf;
	unsigned char got = 0xff;
	int err;

	sf = filp_open(G478_FILE, c->s_rdonly ? O_RDONLY : O_RDWR, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(sf), "case %d: setter open: %ld",
			       ci + 1, PTR_ERR(sf));
	err = g478_lock(sf, false, c->s_posix, c->s_type, c->s_start,
			c->s_len, &g478_setter_files, NULL);
	if (err) {
		filp_close(sf, &g478_setter_files);
		KUNIT_FAIL_AND_ABORT(test, "case %d %s: setlk: %d", ci + 1,
				     g478_variant_names[variant], err);
	}

	switch (variant) {
	case G478_PLAIN:
		/* the child's close of its own copy of the fd */
		get_file(sf);
		filp_close(sf, &g478_child_files);
		break;
	case G478_CLONE_FILES:
		/* the child closes the only fd in the shared table */
		filp_close(sf, &g478_setter_files);
		sf = NULL;
		xfs_settle_fput();
		break;
	case G478_DUP:
		/* dup(fd) then close(dup) */
		get_file(sf);
		filp_close(sf, &g478_setter_files);
		break;
	}

	gf = filp_open(G478_FILE, c->g_rdonly ? O_RDONLY : O_RDWR, 0);
	if (IS_ERR(gf)) {
		if (sf)
			filp_close(sf, &g478_setter_files);
		KUNIT_FAIL_AND_ABORT(test, "case %d: getter open: %ld", ci + 1,
				     PTR_ERR(gf));
	}
	err = g478_lock(gf, true, c->g_posix, c->g_type, c->g_start,
			c->g_len, &g478_getter_files, &got);
	filp_close(gf, &g478_getter_files);
	if (sf)
		filp_close(sf, &g478_setter_files);
	xfs_settle_fput();

	KUNIT_ASSERT_EQ_MSG(test, err, 0, "case %d %s: getlk: %d", ci + 1,
			    g478_variant_names[variant], err);
	KUNIT_EXPECT_EQ_MSG(test, got, c->want[variant],
			    "case %d %s: setter %s %s [%lld,+%lld] %s, getter %s %s [%lld,+%lld] %s: got %s, expected %s",
			    ci + 1, g478_variant_names[variant],
			    c->s_posix ? "posix" : "ofd", g478_type_name(c->s_type),
			    c->s_start, c->s_len, c->s_rdonly ? "RDONLY" : "RDWR",
			    c->g_posix ? "posix" : "ofd", g478_type_name(c->g_type),
			    c->g_start, c->g_len, c->g_rdonly ? "RDONLY" : "RDWR",
			    g478_type_name(got), g478_type_name(c->want[variant]));
}

static void g478_remove_tree(void *unused)
{
	xfs_unlink(G478_FILE);
	xfs_rmdir_settled(G478_ROOT);
}

static void ofd_and_posix_locks_across_owners(struct kunit *test)
{
	u8 *buf;
	int ci, v;

	KUNIT_ASSERT_TRUE(test, xfstests_nfs_mounted());
	KUNIT_ASSERT_EQ(test, xfs_mkdir(G478_ROOT), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, g478_remove_tree, NULL),
			0);

	/* xfs_io -f -c "pwrite -S 0xFF 0 4096" */
	buf = kunit_kmalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	memset(buf, 0xff, 4096);
	KUNIT_ASSERT_EQ(test, xfs_write_new_file(G478_FILE, buf, 4096), 0);

	for (ci = 0; ci < ARRAY_SIZE(g478_table); ci++)
		for (v = 0; v < G478_VARIANTS; v++)
			g478_one(test, ci, v);
}

static int g478_suite_init(struct kunit_suite *suite)
{
	return xfstests_nfs_get();
}

static void g478_suite_exit(struct kunit_suite *suite)
{
	xfstests_nfs_put();
}

static struct kunit_case g478_cases[] = {
	KUNIT_CASE(ofd_and_posix_locks_across_owners),
	{}
};

static struct kunit_suite g478_suite = {
	.name		= "xfstests/generic/478",
	.suite_init	= g478_suite_init,
	.suite_exit	= g478_suite_exit,
	.test_cases	= g478_cases,
};

kunit_test_suites(&g478_suite);

MODULE_DESCRIPTION("xfstests generic/478 over a loopback NFS mount");
MODULE_LICENSE("GPL");
