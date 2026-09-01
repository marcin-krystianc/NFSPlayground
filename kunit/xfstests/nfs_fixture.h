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

/* Where the NFS client mount lives; every port works under this root. */
#define XFS_MNT		"/mnt/nfs"
/* The tmpfs directory knfsd exports (server-side view of XFS_MNT). */
#define XFS_EXPORT	"/export"

int xfstests_nfs_get(void);
void xfstests_nfs_put(void);
bool xfstests_nfs_mounted(void);

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

/* Whole-file convenience wrappers (open/loop/close inside). */
int xfs_write_new_file(const char *path, const void *data, size_t len);
ssize_t xfs_read_range(const char *path, void *buf, size_t len, loff_t off);

#endif /* _XFSTESTS_NFS_FIXTURE_H */
