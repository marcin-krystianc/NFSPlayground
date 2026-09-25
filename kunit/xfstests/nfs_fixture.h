/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The loopback NFS fixture shared by the xfstests ports in fs/xfstests_*.c
 * (sources: kunit/xfstests/). See xfstests_nfs_fixture.c for how the stack
 * is stood up. Suites call xfstests_nfs_get() from suite_init and
 * xfstests_nfs_put() from suite_exit; the first get mounts everything, the
 * last put tears it down, so consecutive suites share one deployment.
 */
#ifndef _XFSTESTS_NFS_FIXTURE_H
#define _XFSTESTS_NFS_FIXTURE_H

#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/statfs.h>
#include <linux/time64.h>
#include <linux/cred.h>

/* Where the NFS client mount lives; every port works under this root. */
#define XFS_MNT		"/mnt/nfs"
/* The tmpfs directory knfsd exports (server-side view of XFS_MNT). */
#define XFS_EXPORT	"/export"

int xfstests_nfs_get(void);
void xfstests_nfs_put(void);
bool xfstests_nfs_mounted(void);

/*
 * Per-test-case fixup: KUnit runs each test case's body in its own fresh
 * kthread (lib/kunit/try_catch.c), which does not share suite_init()'s
 * fs_struct/root -- on current mainline that leaves the fresh thread unable
 * to see anything xfstests_nfs_get()'s bring-up mounted, so its first path
 * lookup under XFS_MNT fails with ENOENT before ever reaching NFS. Wired in
 * as every suite's .init by run-nfs-kunit.sh; see docs/kunit-nfs-reference.md.
 */
struct kunit;
int xfstests_nfs_case_init(struct kunit *test);

/*
 * Override the tmpfs export's mount options for the NEXT bring-up (the
 * fixture re-mounts per suite). The ENOSPC ports use this to get a small
 * filesystem; everyone else gets the default. Reset to the default at
 * teardown automatically.
 */
void xfstests_nfs_export_opts(const char *opts);
#define XFS_EXPORT_OPTS_DEFAULT	"size=67108864,nr_inodes=32768"

/* Remount the NFS client mount read-only / read-write. */
int xfs_remount_client(bool ro);

/*
 * A second client mount of the same export, upstream's SCRATCH_MNT, for
 * ports that need two mounts or a mount cycle. nosharecache gives it its
 * own superblock, so it shares no dentries, inodes or page cache with
 * XFS_MNT. Bring-up does not mount it: a suite mounts it and must call
 * xfs_scratch_umount() before it ends; teardown unmounts leftovers.
 */
#define XFS_SCRATCH_MNT	"/mnt/scratch"
int xfs_scratch_mount(void);
/* Unmount every mount stacked on XFS_SCRATCH_MNT; 0 once none is left. */
int xfs_scratch_umount(void);

/*
 * Static userspace programs from the xfstests submodule (ltp/fsx), built by
 * run-nfs-kunit.sh into a host directory that it names on the kernel
 * command line as xfstests_nfs_fixture.hostbin=<dir>. xfs_run_prog()
 * mounts that directory with hostfs on XFS_HOSTBIN and runs
 * XFS_HOSTBIN/<prog> with call_usermodehelper(), stdout and stderr going to
 * a log file; if the exit status is not 0, the log's last lines are
 * printed with kunit_info(). args is argv without argv[0], NULL-terminated.
 * Returns the exit status (0-255), 128 + the signal if the program was
 * killed, or a negative errno if it could not be run. Safe to call from
 * several threads at once.
 */
#define XFS_HOSTBIN	"/hostbin"
int xfs_run_prog(struct kunit *test, const char *prog,
		 const char *const args[]);

/* mount(2)/umount(2) by path, e.g. for procfs */
int xfs_mount_at(const char *dev, const char *mountpoint, const char *type,
		 const char *opts);
int xfs_umount(const char *mountpoint);

/*
 * Path-based helpers over the fs/namei.c syscall bodies. All return 0 or a
 * negative errno; the filename references are consumed by the callees.
 */
int xfs_mkdir(const char *path);
int xfs_rmdir(const char *path);
/*
 * rmdir that tolerates NFS sillyrename: an unlink/rename-over of an inode
 * whose struct file is still awaiting its delayed fput leaves a transient
 * .nfsXXXX file behind; flush and retry until the sillydelete lands.
 */
int xfs_rmdir_settled(const char *path);
int xfs_unlink(const char *path);
/*
 * Flush the delayed fputs of files this thread has closed, so that a
 * following unlink is a REMOVE rather than a sillyrename to .nfsXXXX.
 * See nfs_fixture.c.
 */
void xfs_settle_fput(void);
int xfs_rename(const char *from, const char *to);
int xfs_link(const char *oldpath, const char *newpath);
int xfs_symlink(const char *target, const char *linkpath);
bool xfs_exists(const char *path);

/* vfs_getattr with AT_STATX_FORCE_SYNC: forces NFS revalidation. */
int xfs_kstat(const char *path, struct kstat *st);
int xfs_truncate(const char *path, loff_t length);
/*
 * ftruncate(2) rather than truncate(2): over NFSv4 the SETATTR carries the
 * open file's stateid, which truncate-by-path cannot. generic/313 needs both.
 */
int xfs_ftruncate(struct file *f, loff_t length);
/* READLINK: the target string itself, NUL-terminated; -ERANGE if it will not fit. */
ssize_t xfs_readlink(const char *path, char *buf, size_t size);

/* Whole-file convenience wrappers (open/loop/close inside). */
int xfs_write_new_file(const char *path, const void *data, size_t len);
ssize_t xfs_read_range(const char *path, void *buf, size_t len, loff_t off);

/*
 * O_DIRECT I/O from a kmalloc'd buffer. kernel_read()/kernel_write() cannot
 * do this over NFS -- their ITER_KVEC reaches iov_iter_get_pages_alloc2(),
 * which returns -EFAULT for a kvec; see nfs_fixture.c. The buffer must come
 * from kmalloc (kunit_kmalloc is fine), not vmalloc.
 */
ssize_t xfs_direct_write(struct file *f, const void *buf, size_t len,
			 loff_t *pos);
ssize_t xfs_direct_read(struct file *f, void *buf, size_t len, loff_t *pos);

/*
 * Writes sourced from a user address, for the ports that write out of a
 * kunit_vm_mmap() mapping. The iovec form's iov_base values are user
 * pointers; the array itself is an ordinary kernel one.
 */
struct iovec;
ssize_t xfs_user_write(struct file *f, const void __user *buf, size_t len,
		       loff_t *pos);
/* the general form: reads too, and either the buffered or the direct path */
ssize_t xfs_user_rw(struct file *f, void __user *buf, size_t len, loff_t *pos,
		    bool write, bool direct);
ssize_t xfs_user_writev(struct file *f, const struct iovec *iov,
			unsigned long nr_segs, loff_t *pos);

/* mknod(2): character/block/fifo/socket nodes, built from kern_path_create */
int xfs_mknod(const char *path, umode_t mode, unsigned int major,
	      unsigned int minor);

int xfs_statfs(const char *path, struct kstatfs *st);
/* poll until XFS_MNT reports at least this many bytes available */
int xfs_wait_for_free_bytes(u64 bytes);
int xfs_fsync_path(const char *path);
int xfs_chmod(const char *path, umode_t mode);
int xfs_chown(const char *path, uid_t uid, gid_t gid);
/* SETATTR of atime/mtime, both set explicitly (nsec 0). */
int xfs_utimes(const char *path, time64_t atime, time64_t mtime);
/*
 * The same SETATTR with the timespecs given verbatim, so a caller can pass
 * UTIME_OMIT or UTIME_NOW in tv_nsec -- generic/221's case is atime set with
 * mtime omitted.
 */
int xfs_utimes_raw(const char *path, struct timespec64 times[2]);

/*
 * Run as another user: switches fsuid/euid/... AND drops capabilities so
 * DAC checks actually apply. Serial use only (tests run one at a time).
 */
int xfs_switch_creds(uid_t uid, gid_t gid);
void xfs_restore_creds(void);

/*
 * seteuid(2)'s actual effect: only euid/fsuid move, uid/suid and
 * capabilities are untouched by hand -- security_task_fix_setuid() decides
 * what happens to cap_effective, the same LSM hook setresuid(2) goes
 * through. Pairs with xfs_restore_creds(), same one-at-a-time rule as
 * xfs_switch_creds().
 */
int xfs_seteuid(uid_t uid);

/*
 * access(2)'s in-kernel equivalent: POSIX requires the check to use the
 * *real* uid/gid, not the effective ones, so this builds the same
 * override cred access(2) itself builds (fs/open.c's
 * access_override_creds()) before calling inode_permission() -- fsuid set
 * to the real uid, and capabilities restored to cap_permitted if that
 * real uid is 0, cleared otherwise. `mode` is R_OK/W_OK/X_OK, OR'd.
 */
int xfs_access(const char *path, int mode);

/* xattrs by path, with the mnt_want_write dance callers of vfs_* owe */
int xfs_setxattr(const char *path, const char *name, const void *value,
		 size_t size, int flags);
ssize_t xfs_getxattr(const char *path, const char *name, void *value,
		     size_t size);
ssize_t xfs_listxattr(const char *path, char *list, size_t size);
int xfs_removexattr(const char *path, const char *name);

/*
 * POSIX advisory lock via vfs_lock_file: type is F_RDLCK/F_WRLCK/F_UNLCK,
 * owner distinguishes lockowners (each becomes an NFSv4 lockowner).
 */
int xfs_posix_lock(struct file *f, unsigned char type, loff_t start,
		   loff_t end, fl_owner_t owner, bool wait);

#endif /* _XFSTESTS_NFS_FIXTURE_H */
