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
int xfs_rename(const char *from, const char *to);
int xfs_link(const char *oldpath, const char *newpath);
int xfs_symlink(const char *target, const char *linkpath);
int xfs_mknod_chr(const char *path);
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
