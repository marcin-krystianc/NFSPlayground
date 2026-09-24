// SPDX-License-Identifier: GPL-2.0
/*
 * A complete NFS deployment inside one UML kernel, shared by the xfstests
 * ports: tmpfs on /export, served by knfsd on 127.0.0.1:2049, mounted back
 * as NFSv4.2 on /mnt/nfs. Every file operation a test makes under /mnt/nfs
 * is a real RPC round-trip through fs/nfs and net/sunrpc, served by
 * fs/nfsd, over loopback TCP.
 *
 * The UML kernel has no userspace, so the fixture does the jobs userspace
 * normally does:
 *
 *  - brings the loopback interface up (the kernel auto-assigns 127.0.0.1
 *    to a loopback device on UP)
 *  - mounts the nfsd control filesystem: its fill_super populates
 *    nn->nfsd_client_dir, which create_client() dereferences on the first
 *    EXCHANGE_ID -- without it the first client connection panics knfsd
 *  - feeds the three sunrpc caches rpc.mountd would write (auth.unix.ip,
 *    nfsd.export, nfsd.fh) by calling their parse functions directly,
 *    un-staticed by scripts/kunit/run-nfs-kunit.sh
 *  - starts one knfsd thread, v4-only (no lockd, no rpcbind), and ends
 *    the 90-second v4 grace period the way /proc/fs/nfsd/v4_end_grace does
 *
 * The client needs no userspace at all for v4.2 with sec=sys: no rpcbind,
 * no statd, and numeric IDs (nfs4_disable_idmapping defaults on) mean no
 * idmapd.
 *
 * Bring-up is refcounted: suites call xfstests_nfs_get()/put() from
 * suite_init/suite_exit, so consecutive xfstests suites share one
 * deployment and the last one out turns off the lights. Teardown failures
 * are pr_err'd rather than asserted (there is no struct kunit here); the
 * teardown path was assertion-verified when it lived inside the
 * generic/001 suite.
 *
 * Honest scope: client and server share one kernel and one page cache, so
 * cross-client cache coherence and crash consistency are out of reach, and
 * the server is knfsd on tmpfs, not any production server. v3 would need
 * the userspace mountd protocol, so the fixture is v4-only by construction.
 */

#include <kunit/test.h>
#include <kunit/visibility.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/bvec.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <uapi/linux/mount.h>	/* MS_REMOUNT, MS_RDONLY */
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/cache.h>
#include <linux/statfs.h>
#include <linux/xattr.h>
#include <linux/filelock.h>
#include <linux/cred.h>
#include <linux/fs_struct.h>	/* set_fs_root/set_fs_pwd, see xfstests_nfs_case_init() */
#include <linux/capability.h>
#include <linux/uidgid.h>
#include <linux/security.h>	/* security_task_fix_setuid, see xfs_seteuid() */
#include <linux/dirent.h>	/* struct linux_dirent64, see xfs_getdents() */
#include <linux/mman.h>		/* see xfs_holetest() */
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched/mm.h>
#include <linux/falloc.h>

#include "internal.h"		/* path_mount/path_umount, do_*at bodies */
#include "nfsd/nfsd.h"		/* nfsd_svc, nfsd_vers, nfsd_mutex */
#include "nfsd/netns.h"		/* struct nfsd_net, nfsd_net_id */
#include "nfsd/state.h"		/* nfsd4_end_grace */
#include "../net/sunrpc/netns.h"	/* struct sunrpc_net, sunrpc_net_id */

#include "xfstests_nfs_fixture.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* mountd's cache writers; private to their files, un-staticed by the runner. */
int ip_map_parse(struct cache_detail *cd, char *mesg, int mlen);
int svc_export_parse(struct cache_detail *cd, char *mesg, int mlen);
int expkey_parse(struct cache_detail *cd, char *mesg, int mlen);
/*
 * Public via fs/nfsd/state.h (included above) through v6.12.57; became
 * file-private on current mainline and un-staticed by the runner, same as
 * the ip_map_parse family above. state.h no longer declares it either way,
 * so this stays needed on both: redundant but harmless where it is still
 * public, load-bearing where it is not.
 */
void nfsd4_end_grace(struct nfsd_net *nn);
/* fs/open.c bodies; non-static there but not declared in a header */
int chmod_common(const struct path *path, umode_t mode);
int chown_common(const struct path *path, uid_t user, gid_t group);
/*
 * mknod(2)'s syscall body. File-private on v6.12.57 (un-staticed by the
 * runner) and undeclared there; on current mainline it is renamed
 * filename_mknodat() and already declared in the fs/internal.h included
 * above, where this repeats it harmlessly. The runner rewrites the name in
 * this file's copy along with the rest of the fs/namei.c renames.
 */
int do_mknodat(int dfd, struct filename *name, umode_t mode, unsigned int dev);

#define XFS_NFSDFS	"/nfsdfs"
#define XFS_DOMAIN	"localhost"
/* NFSEXP_INSECURE_PORT | NFSEXP_NOSUBTREECHECK | NFSEXP_FSID */
#define XFS_EXPFLAGS	0x2402

/*
 * ---------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------
 */

int xfs_mkdir(const char *path)
{
	return do_mkdirat(AT_FDCWD, getname_kernel(path), 0755);
}

int xfs_rmdir(const char *path)
{
	return do_rmdir(AT_FDCWD, getname_kernel(path));
}

int xfs_unlink(const char *path)
{
	return do_unlinkat(AT_FDCWD, getname_kernel(path));
}

int xfs_rename(const char *from, const char *to)
{
	return do_renameat2(AT_FDCWD, getname_kernel(from),
			    AT_FDCWD, getname_kernel(to), 0);
}

int xfs_link(const char *oldpath, const char *newpath)
{
	return do_linkat(AT_FDCWD, getname_kernel(oldpath),
			 AT_FDCWD, getname_kernel(newpath), 0);
}

int xfs_symlink(const char *target, const char *linkpath)
{
	return do_symlinkat(getname_kernel(target), AT_FDCWD,
			    getname_kernel(linkpath));
}

bool xfs_exists(const char *path)
{
	struct path p;

	if (kern_path(path, 0, &p))
		return false;
	path_put(&p);
	return true;
}

int xfs_kstat(const char *path, struct kstat *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	/* FORCE_SYNC makes the NFS client revalidate against the server */
	err = vfs_getattr(&p, st, STATX_BASIC_STATS, AT_STATX_FORCE_SYNC);
	path_put(&p);
	return err;
}

int xfs_ftruncate(struct file *f, loff_t length)
{
	/* ftruncate(2)'s body: SETATTR carrying the open file's stateid */
	return do_ftruncate(f, length, 0);
}

ssize_t xfs_readlink(const char *path, char *buf, size_t size)
{
	DEFINE_DELAYED_CALL(done);
	const char *target;
	struct path p;
	size_t len;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	/* vfs_readlink() wants a __user buffer; vfs_get_link() hands us the
	 * string itself, which is what an in-kernel caller can compare.
	 */
	target = vfs_get_link(p.dentry, &done);
	if (IS_ERR(target)) {
		path_put(&p);
		return PTR_ERR(target);
	}
	len = strlen(target);
	if (len >= size) {
		do_delayed_call(&done);
		path_put(&p);
		return -ERANGE;
	}
	memcpy(buf, target, len + 1);
	do_delayed_call(&done);
	path_put(&p);
	return len;
}

int xfs_truncate(const char *path, loff_t length)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_truncate(&p, length);
	path_put(&p);
	return err;
}

int xfs_write_new_file(const char *path, const void *data, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t written;
	int err = 0;

	/* O_LARGEFILE by hand: an in-kernel open does not get
	 * force_o_largefile(), so without it these helpers stop at
	 * MAX_NON_LFS (2 GiB) -- see the generic/308 and 525 ports.
	 */
	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	if (IS_ERR(f))
		return PTR_ERR(f);
	while (len) {
		written = kernel_write(f, data, len, &pos);
		if (written <= 0) {
			err = written ? (int)written : -EIO;
			break;
		}
		data += written;
		len -= written;
	}
	filp_close(f, NULL);
	return err;
}

ssize_t xfs_read_range(const char *path, void *buf, size_t len, loff_t off)
{
	struct file *f;
	ssize_t got;

	f = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	got = kernel_read(f, buf, len, &off);
	filp_close(f, NULL);
	return got;
}

/*
 * O_DIRECT from a kernel buffer.
 *
 * kernel_read()/kernel_write() build an ITER_KVEC, and NFS's direct path
 * ends up in iov_iter_get_pages_alloc2() (fs/nfs/direct.c ->
 * nfs_direct_{read,write}_schedule_iovec), which handles user-backed, bvec,
 * folioq and xarray iterators and returns -EFAULT for everything else,
 * kvec included (lib/iov_iter.c, __iov_iter_get_pages_alloc). So an
 * O_DIRECT kernel_write() over NFS fails with -EFAULT before any RPC is
 * sent -- confirmed here, not assumed.
 *
 * An ITER_BVEC over the same memory is the way in: the buffer must be
 * kmalloc'd (physically contiguous, so one bio_vec covers it and the
 * consecutive struct pages the bvec path walks are the right ones).
 * IOCB_DIRECT is set explicitly so this is the direct path whether or not
 * the caller opened with O_DIRECT.
 */
static ssize_t xfs_direct_rw(struct file *f, void *buf, size_t len,
			     loff_t *pos, bool write)
{
	struct iov_iter iter;
	struct bio_vec bv;
	struct kiocb kiocb;
	ssize_t ret;

	bvec_set_page(&bv, virt_to_page(buf), len, offset_in_page(buf));
	iov_iter_bvec(&iter, write ? ITER_SOURCE : ITER_DEST, &bv, 1, len);

	init_sync_kiocb(&kiocb, f);
	kiocb.ki_pos = *pos;
	kiocb.ki_flags |= IOCB_DIRECT;

	if (write)
		ret = vfs_iocb_iter_write(f, &kiocb, &iter);
	else
		ret = vfs_iocb_iter_read(f, &kiocb, &iter);
	if (ret > 0)
		*pos = kiocb.ki_pos;
	return ret;
}

ssize_t xfs_direct_write(struct file *f, const void *buf, size_t len,
			 loff_t *pos)
{
	return xfs_direct_rw(f, (void *)buf, len, pos, true);
}

/*
 * Writes whose source is a user address, i.e. somewhere inside a
 * kunit_vm_mmap() mapping. kernel_write() cannot express this: its
 * ITER_KVEC would have the copy read the user pointer as a kernel one.
 * The iovec form takes a kernel array of iovecs whose iov_base values are
 * user pointers, which is exactly what ITER_IOVEC is.
 */
static ssize_t xfs_user_rw_iter(struct file *f, struct iov_iter *iter,
				loff_t *pos, bool write, bool direct)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, f);
	kiocb.ki_pos = *pos;
	if (direct)
		kiocb.ki_flags |= IOCB_DIRECT;
	if (write)
		ret = vfs_iocb_iter_write(f, &kiocb, iter);
	else
		ret = vfs_iocb_iter_read(f, &kiocb, iter);
	if (ret > 0)
		*pos = kiocb.ki_pos;
	return ret;
}

ssize_t xfs_user_rw(struct file *f, void __user *buf, size_t len, loff_t *pos,
		    bool write, bool direct)
{
	struct iov_iter iter;

	iov_iter_ubuf(&iter, write ? ITER_SOURCE : ITER_DEST, buf, len);
	return xfs_user_rw_iter(f, &iter, pos, write, direct);
}

ssize_t xfs_user_write(struct file *f, const void __user *buf, size_t len,
		       loff_t *pos)
{
	return xfs_user_rw(f, (void __user *)buf, len, pos, true, false);
}

ssize_t xfs_user_writev(struct file *f, const struct iovec *iov,
			unsigned long nr_segs, loff_t *pos)
{
	struct iov_iter iter;
	size_t count = 0;
	unsigned long i;

	for (i = 0; i < nr_segs; i++)
		count += iov[i].iov_len;
	iov_iter_init(&iter, ITER_SOURCE, iov, nr_segs, count);
	return xfs_user_rw_iter(f, &iter, pos, true, false);
}

ssize_t xfs_direct_read(struct file *f, void *buf, size_t len, loff_t *pos)
{
	return xfs_direct_rw(f, buf, len, pos, false);
}

int xfs_mknod(const char *path, umode_t mode, unsigned int major,
	      unsigned int minor)
{
	return do_mknodat(AT_FDCWD, getname_kernel(path), mode,
			  new_encode_dev(MKDEV(major, minor)));
}

int xfs_statfs(const char *path, struct kstatfs *st)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = vfs_statfs(&p, st);
	path_put(&p);
	return err;
}

int xfs_fsync_path(const char *path)
{
	struct file *f;
	int err;

	f = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	err = vfs_fsync(f, 0);
	filp_close(f, NULL);
	return err;
}

int xfs_chmod(const char *path, umode_t mode)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = chmod_common(&p, mode);
	path_put(&p);
	return err;
}

int xfs_chown(const char *path, uid_t uid, gid_t gid)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = chown_common(&p, uid, gid);
	path_put(&p);
	return err;
}

int xfs_utimes_raw(const char *path, struct timespec64 times[2])
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	/* vfs_utimes() reads UTIME_OMIT/UTIME_NOW out of tv_nsec itself */
	err = vfs_utimes(&p, times);
	path_put(&p);
	return err;
}

int xfs_utimes(const char *path, time64_t atime, time64_t mtime)
{
	struct timespec64 times[2] = {
		{ .tv_sec = atime, .tv_nsec = 0 },
		{ .tv_sec = mtime, .tv_nsec = 0 },
	};

	return xfs_utimes_raw(path, times);
}

/*
 * Credentials switching. NFS with sec=sys puts current fsuid/fsgid on the
 * wire, and the client's own permission checks (ACCESS) use current creds
 * too -- but only if capabilities are dropped, since CAP_DAC_OVERRIDE
 * bypasses DAC entirely.
 */
static const struct cred *xfs_saved_creds;
static struct cred *xfs_override;

int xfs_switch_creds(uid_t uid, gid_t gid)
{
	struct cred *c;

	if (WARN_ON(xfs_saved_creds))
		return -EBUSY;
	c = prepare_creds();
	if (!c)
		return -ENOMEM;
	c->uid = c->euid = c->suid = c->fsuid = KUIDT_INIT(uid);
	c->gid = c->egid = c->sgid = c->fsgid = KGIDT_INIT(gid);
	cap_clear(c->cap_inheritable);
	cap_clear(c->cap_permitted);
	cap_clear(c->cap_effective);
	xfs_override = c;
	xfs_saved_creds = override_creds(c);
	return 0;
}

void xfs_restore_creds(void)
{
	if (!xfs_saved_creds)
		return;
	revert_creds(xfs_saved_creds);
	put_cred(xfs_override);
	xfs_saved_creds = NULL;
	xfs_override = NULL;
}

int xfs_seteuid(uid_t uid)
{
	struct cred *c;
	const struct cred *old;
	int err;

	if (WARN_ON(xfs_saved_creds))
		return -EBUSY;
	old = current_cred();
	c = prepare_creds();
	if (!c)
		return -ENOMEM;
	/* setresuid(-1, uid, -1): only euid (and fsuid, which follows it) move */
	c->euid = c->fsuid = KUIDT_INIT(uid);
	err = security_task_fix_setuid(c, old, LSM_SETID_RES);
	if (err) {
		abort_creds(c);
		return err;
	}
	xfs_override = c;
	xfs_saved_creds = override_creds(c);
	return 0;
}

/*
 * fs/open.c's access_override_creds(), reproduced: fsuid/fsgid are pinned
 * to the *real* uid/gid for the duration of the check, and capabilities
 * are restored to cap_permitted if that real uid is 0 (root regains
 * CAP_DAC_OVERRIDE for the check even if its effective uid currently
 * isn't root) or cleared otherwise. do_faccessat() only builds this
 * override when fsuid/uid already disagree; this always builds it, which
 * changes nothing about the result -- inode_permission() sees the same
 * cred either way -- and keeps xfs_access() self-contained.
 *
 * The single put_cred() below, after revert_creds() rather than right
 * after override_creds(), is deliberate: some kernel versions have
 * override_creds()/revert_creds() take and drop an extra reference
 * themselves (the historical contract this mirrors, fs/open.c's own
 * access_override_creds(), relies on that extra ref to justify its own
 * immediate put_cred()), others (newer cred.h, where both are a bare
 * rcu_replace_pointer() with no refcounting at all) don't. Exactly one
 * put_cred() to match exactly one prepare_creds() -- the same shape
 * xfs_switch_creds()/xfs_restore_creds() already use -- balances either
 * way; a put_cred() sandwiched between override_creds() and revert_creds()
 * does not, and frees the cred while it is still current->cred on a
 * no-extra-ref kernel (kernel/cred.c: "BUG_ON(cred == current->cred)").
 */
int xfs_access(const char *path, int mode)
{
	const struct cred *old_cred;
	struct cred *override;
	struct path p;
	int err;

	override = prepare_creds();
	if (!override)
		return -ENOMEM;
	override->fsuid = override->uid;
	override->fsgid = override->gid;
	if (uid_eq(override->uid, GLOBAL_ROOT_UID))
		override->cap_effective = override->cap_permitted;
	else
		cap_clear(override->cap_effective);
	old_cred = override_creds(override);

	err = kern_path(path, LOOKUP_FOLLOW, &p);
	if (!err) {
		err = inode_permission(mnt_idmap(p.mnt),
				       d_backing_inode(p.dentry),
				       mode | MAY_ACCESS);
		path_put(&p);
	}

	revert_creds(old_cred);
	put_cred(override);
	return err;
}

int xfs_setxattr(const char *path, const char *name, const void *value,
		 size_t size, int flags)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = mnt_want_write(p.mnt);
	if (!err) {
		err = vfs_setxattr(&nop_mnt_idmap, p.dentry, name, value,
				   size, flags);
		mnt_drop_write(p.mnt);
	}
	path_put(&p);
	return err;
}

ssize_t xfs_getxattr(const char *path, const char *name, void *value,
		     size_t size)
{
	struct path p;
	ssize_t ret;

	ret = kern_path(path, 0, &p);
	if (ret)
		return ret;
	ret = vfs_getxattr(&nop_mnt_idmap, p.dentry, name, value, size);
	path_put(&p);
	return ret;
}

ssize_t xfs_listxattr(const char *path, char *list, size_t size)
{
	struct path p;
	ssize_t ret;

	ret = kern_path(path, 0, &p);
	if (ret)
		return ret;
	ret = vfs_listxattr(p.dentry, list, size);
	path_put(&p);
	return ret;
}

int xfs_removexattr(const char *path, const char *name)
{
	struct path p;
	int err;

	err = kern_path(path, 0, &p);
	if (err)
		return err;
	err = mnt_want_write(p.mnt);
	if (!err) {
		err = vfs_removexattr(&nop_mnt_idmap, p.dentry, name);
		mnt_drop_write(p.mnt);
	}
	path_put(&p);
	return err;
}

/*
 * xfstests' lib/random.c, which src/Makefile links into every src/ program
 * ahead of libc, so upstream's nametest, dirstress, truncfile... draw from
 * this generator rather than glibc's. Reproducing it is what lets a port
 * replay upstream's exact sequence for a given seed (generic/007's golden
 * counts depend on it). The arithmetic is u32 so it wraps the way the
 * original's int32_t does when built without strict-overflow assumptions.
 */
static const s32 xfs_random_mt[128] = {
	902906369, 2030498053, -473499623, 1640834941,
	723406961, 1993558325, -257162999, -1627724755,
	913952737, 278845029, 1327502073, -1261253155,
	981676113, -1785280363, 1700077033, 366908557,
	-1514479167, -682799163, 141955545, -830150595,
	317871153, 1542036469, -946413879, -1950779155,
	985397153, 626515237, 530871481, 783087261,
	-1512358895, 1031357269, -2007710807, -1652747955,
	-1867214463, 928251525, 1243003801, -2132510467,
	1874683889, -717013323, 218254473, -1628774995,
	-2064896159, 69678053, 281568889, -2104168611,
	-165128239, 1536495125, -39650967, 546594317,
	-725987007, 1392966981, 1044706649, 687331773,
	-2051306575, 1544302965, -758494647, -1243934099,
	-75073759, 293132965, -1935153095, 118929437,
	807830417, -1416222507, -1550074071, -84903219,
	1355292929, -380482555, -1818444007, -204797315,
	170442609, -1636797387, 868931593, -623503571,
	1711722209, 381210981, -161547783, -272740131,
	-1450066095, 2116588437, 1100682473, 358442893,
	-1529216831, 2116152005, -776333095, 1265240893,
	-482278607, 1067190005, 333444553, 86502381,
	753481377, 39000101, 1779014585, 219658653,
	-920253679, 2029538901, 1207761577, -1515772851,
	-236195711, 442620293, 423166617, -1763648515,
	-398436623, -1749358155, -538598519, -652439379,
	430550625, -1481396507, 2093206905, -1934691747,
	-962631983, 1454463253, -1877118871, -291917555,
	-1711673279, 201201733, -474645415, -96764739,
	-1587365199, 1945705589, 1303896393, 1744831853,
	381957665, 2135332261, -55996615, -1190135011,
	1790562961, -1493191723, 475559465, 69069
};

static s32 xfs_irandm(struct xfs_random *r)
{
	s32 it = r->is[0], leh = r->is[1], nit;

	if (it <= 0)
		it = (s32)(((u32)it + (u32)it) ^ 593970775u);
	else
		it = (s32)((u32)it + (u32)it);
	nit = (s32)((u32)it - 1);
	leh = (s32)((u32)leh * (u32)xfs_random_mt[nit & 127] + (u32)nit);
	r->is[0] = it;
	r->is[1] = leh;
	if (leh < 0)
		leh = ~leh;
	return leh;
}

void xfs_srandom(struct xfs_random *r, unsigned int seed)
{
	r->is[0] = seed;
	r->is[1] = 0;
	xfs_irandm(r);
}

long xfs_random(struct xfs_random *r)
{
	return xfs_irandm(r);
}

/*
 * getdents64(2) over iterate_dir(), batching the way fs/readdir.c's
 * filldir64() does: a record takes ALIGN(offsetof(d_name) + namlen + 1, 8)
 * bytes of the caller's buffer, a record that does not fit ends the batch
 * (-EINVAL if it is the first), each record's d_off is the offset of the
 * record after it, and the last one's is the directory position the call
 * leaves behind.
 */
struct xfs_getdents_ctx {
	struct dir_context	ctx;
	struct xfs_dirent	*ents;
	int			max, n;
	size_t			left;
	int			error;
};

static bool xfs_getdents_actor(struct dir_context *ctx, const char *name,
			       int namlen, loff_t offset, u64 ino,
			       unsigned int d_type)
{
	struct xfs_getdents_ctx *g = container_of(ctx, struct xfs_getdents_ctx,
						  ctx);
	size_t reclen = ALIGN(offsetof(struct linux_dirent64, d_name) +
			      namlen + 1, sizeof(u64));
	struct xfs_dirent *e;

	if (reclen > g->left || g->n == g->max) {
		if (!g->n)
			g->error = -EINVAL;
		return false;
	}
	if (g->n)
		g->ents[g->n - 1].d_off = offset;
	e = &g->ents[g->n++];
	namlen = min_t(int, namlen, NAME_MAX);
	memcpy(e->name, name, namlen);
	e->name[namlen] = '\0';
	e->ino = ino;
	e->type = d_type;
	g->left -= reclen;
	return true;
}

int xfs_getdents(struct file *dir, struct xfs_dirent *ents, int max,
		 size_t bufsize)
{
	struct xfs_getdents_ctx g = {
		.ctx.actor = xfs_getdents_actor,
		.ents = ents,
		.max = max,
		.left = bufsize,
	};
	int err = iterate_dir(dir, &g.ctx);

	if (g.n)
		ents[g.n - 1].d_off = g.ctx.pos;
	if (err)
		return err;
	return g.n ? g.n : g.error;
}

size_t xfs_libc_dirbuf(struct file *dir)
{
	struct kstat st;

	/* opendir() fails if its fstat() does */
	if (vfs_getattr(&dir->f_path, &st, STATX_BASIC_STATS,
			AT_STATX_SYNC_AS_STAT))
		return 0;
	return max_t(size_t, st.blksize, 32768);
}

/*
 * src/t_dir_offset2 <dir> [bufsize [+name|-name]]: read the directory
 * with getdents64 in bufsize batches, recording every entry's d_off and
 * d_ino and failing on a d_off seen twice; with a name, create ("+") or
 * unlink ("-") it after the first batch, keep going on the old descriptor,
 * then walk a descriptor opened after the change, which must show the
 * entry iff it exists (with the inode it had, if it existed before);
 * finally lseek to each recorded d_off from the last entry back and check
 * that the next getdents starts with the entry recorded there.
 */
#define XFS_TDO2_HISTORY	1024	/* HISTORY_LEN */

void xfs_t_dir_offset2(struct kunit *test, const char *dir, size_t bufsize,
		       const char *arg)
{
	loff_t *off_hist;
	u64 *ino_hist;
	struct xfs_dirent *ents;
	struct file *fd, *fd2 = NULL, *first;
	const char *filename = NULL;
	char *path;
	struct kstat st = {};
	bool exists = false, found = false;
	int max = bufsize / 24 + 1, modify = 0;
	int total = 0, n, i, j;

	off_hist = kunit_kcalloc(test, XFS_TDO2_HISTORY, sizeof(*off_hist),
				 GFP_KERNEL);
	ino_hist = kunit_kcalloc(test, XFS_TDO2_HISTORY, sizeof(*ino_hist),
				 GFP_KERNEL);
	ents = kunit_kcalloc(test, max, sizeof(*ents), GFP_KERNEL);
	path = kunit_kzalloc(test, PATH_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, off_hist);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ino_hist);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ents);

	if (arg) {
		filename = arg;
		if (arg[0] == '+' || arg[0] == '-') {
			modify = arg[0] == '+' ? 1 : -1;
			filename++;
		}
		snprintf(path, PATH_MAX, "%s/%s", dir, filename);
		exists = !xfs_kstat(path, &st);
	}

	first = fd = filp_open(dir, O_RDONLY | O_DIRECTORY, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(fd), "open %s: %ld", dir,
			       PTR_ERR(fd));
	for (;;) {
		n = xfs_getdents(fd, ents, max, bufsize);
		if (n < 0) {
			KUNIT_FAIL(test, "getdents: %d", n);
			goto out;
		}
		if (modify && !fd2 && total == 0) {
			/* create/unlink entry after first getdents */
			if (modify > 0) {
				KUNIT_EXPECT_EQ_MSG(test,
						    xfs_write_new_file(path, "", 0),
						    0, "openat %s", path);
				exists = true;
			} else {
				KUNIT_EXPECT_EQ_MSG(test, xfs_unlink(path), 0,
						    "unlinkat %s", path);
				exists = false;
			}
			/* keep the old fd; walk a new one for stale or missing */
			fd2 = filp_open(dir, O_RDONLY | O_DIRECTORY, 0);
			if (IS_ERR(fd2)) {
				KUNIT_FAIL(test, "open fd2: %ld", PTR_ERR(fd2));
				fd2 = NULL;
				goto out;
			}
		}
		if (n == 0) {
			if (!fd2 || fd == fd2)
				break;
			/* re-iterate with the new fd, leaving the old one open */
			fd = fd2;
			total = 0;
			found = false;
			continue;
		}
		for (i = 0; i < n; i++, total++) {
			if (total >= XFS_TDO2_HISTORY) {
				KUNIT_FAIL(test, "too many files");
				break;
			}
			for (j = 0; j < total; j++)
				KUNIT_EXPECT_NE_MSG(test, off_hist[j], ents[i].d_off,
						    "entries %d and %d have duplicate d_off %lld",
						    j, total, ents[i].d_off);
			off_hist[total] = ents[i].d_off;
			ino_hist[total] = ents[i].ino;
			if (filename && !strcmp(filename, ents[i].name)) {
				found = true;
				if (st.ino)
					KUNIT_EXPECT_EQ_MSG(test, ents[i].ino,
							    st.ino,
							    "entry %s has inode %llu, expected %llu",
							    filename, ents[i].ino,
							    st.ino);
			}
		}
	}

	if (filename)
		KUNIT_EXPECT_EQ_MSG(test, found, exists, "%s entry %s",
				    exists ? "missing" : "stale", filename);

	/* check if seek works correctly */
	for (i = total - 1; i >= 0; i--) {
		loff_t pos = i > 0 ? off_hist[i - 1] : 0;

		KUNIT_EXPECT_EQ_MSG(test, vfs_llseek(fd, pos, SEEK_SET), pos,
				    "lseek to %lld", pos);
		n = xfs_getdents(fd, ents, max, bufsize);
		if (n <= 0) {
			KUNIT_FAIL(test, "getdents returned %d on entry %d", n,
				   i);
			continue;
		}
		KUNIT_EXPECT_EQ_MSG(test, ents[0].ino, ino_hist[i],
				    "entry %d has inode %llu, expected %llu",
				    i, ents[0].ino, ino_hist[i]);
	}
out:
	if (fd2)
		filp_close(fd2, NULL);
	filp_close(first, NULL);
	kunit_kfree(test, path);
	kunit_kfree(test, ents);
	kunit_kfree(test, ino_hist);
	kunit_kfree(test, off_hist);
}

/*
 * src/holetest, one size, as its main() runs it: the file is sized three
 * ways in turn -- ftruncate plus an explicit zero fill through a shared
 * mapping, posix_fallocate, plain ftruncate -- and each time test_this()
 * maps it, starts two threads that write their own id at their own offset
 * (a quarter and three quarters into each page), waits, and checks every
 * page holds both ids. -w makes thread 0 use pwrite instead of the
 * mapping; -r reads every page first (prefault); -p maps MAP_PRIVATE, and
 * then also remaps the file read-only to check no write reached it.
 *
 * The threads are kthreads borrowing this thread's mm. Beyond upstream, a
 * shared run also reads both ids back from the server's copy of the file
 * after the mapping is gone and the file closed.
 */
#define XFS_HOLETEST_THREADS	2

struct xfs_holetest_marker {
	struct mm_struct	*mm;
	struct file		*f;
	unsigned long		va;
	unsigned long		npages;
	unsigned long		pgoff;
	u64			id;
	bool			use_wr;
	int			err;
	struct completion	done;
};

/* pt_page_marker() and pt_write_marker() */
static int xfs_holetest_mark(void *arg)
{
	struct xfs_holetest_marker *m = arg;
	unsigned long i;

	if (!m->use_wr)
		kthread_use_mm(m->mm);
	for (i = 0; i < m->npages && !m->err; i++) {
		if (m->use_wr) {
			loff_t pos = (loff_t)i * PAGE_SIZE + m->pgoff;

			if (kernel_write(m->f, &m->id, sizeof(m->id), &pos) !=
			    sizeof(m->id))
				m->err = -EIO;
		} else if (copy_to_user((void __user *)(m->va + i * PAGE_SIZE +
							m->pgoff),
					&m->id, sizeof(m->id))) {
			m->err = -EFAULT;
		}
	}
	if (!m->use_wr)
		kthread_unuse_mm(m->mm);
	complete(&m->done);
	return 0;
}

/* verify_mapping(): the number of slots that do not hold their id */
static int xfs_holetest_verify(struct kunit *test, unsigned long va,
			       unsigned long npages, const u64 *expect,
			       const char *what)
{
	int errcnt = 0, i;
	unsigned long p;
	u64 v;

	for (p = 0; p < npages; p++) {
		for (i = 0; i < XFS_HOLETEST_THREADS; i++) {
			unsigned long off = (PAGE_SIZE / 4) * (2 * i + 1);

			if (copy_from_user(&v, (void __user *)(va + p * PAGE_SIZE +
							       off),
					   sizeof(v)))
				v = ~expect[i];
			if (v != expect[i] && errcnt++ < 3)
				KUNIT_FAIL(test,
					   "%s: thread %d, offset %08lx, %08llx != %08llx",
					   what, i, p * PAGE_SIZE + off, v,
					   expect[i]);
		}
	}
	return errcnt;
}

/* test_this() */
static void xfs_holetest_this(struct kunit *test, struct file *f,
			      loff_t sz, unsigned int flags, const char *what)
{
	bool private = flags & XFS_HOLETEST_PRIVATE;
	struct xfs_holetest_marker m[XFS_HOLETEST_THREADS] = {};
	u64 tid[XFS_HOLETEST_THREADS], zero[XFS_HOLETEST_THREADS] = {};
	unsigned long npages = sz / PAGE_SIZE, va, p;
	struct task_struct *t;
	u8 c;
	int i;

	va = kunit_vm_mmap(test, f, 0, sz, PROT_READ | PROT_WRITE,
			   private ? MAP_PRIVATE : MAP_SHARED, 0);
	KUNIT_ASSERT_NE_MSG(test, va, 0UL, "%s: mmap()", what);

	if (flags & XFS_HOLETEST_PREFAULT)
		for (p = 0; p < npages; p++)
			if (copy_from_user(&c, (void __user *)(va + p * PAGE_SIZE),
					   1) || c)
				KUNIT_FAIL(test,
					   "%s: prefaulting found non-zero value in page %lu",
					   what, p);

	for (i = 0; i < XFS_HOLETEST_THREADS; i++) {
		tid[i] = 0x4f4c4500000000ULL | (i + 1);
		m[i] = (struct xfs_holetest_marker){
			.mm = current->mm,
			.f = f,
			.va = va,
			.npages = npages,
			.pgoff = (PAGE_SIZE / 4) * (2 * i + 1),
			.id = tid[i],
			.use_wr = i == 0 && (flags & XFS_HOLETEST_WRITE),
		};
		init_completion(&m[i].done);
		t = kthread_run(xfs_holetest_mark, &m[i], "holetest-%d", i);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(t), "%s: thread %d: %ld",
				       what, i, PTR_ERR(t));
	}
	for (i = 0; i < XFS_HOLETEST_THREADS; i++) {
		wait_for_completion(&m[i].done);
		KUNIT_EXPECT_EQ_MSG(test, m[i].err, 0, "%s: thread %d failed",
				    what, i);
	}

	KUNIT_EXPECT_EQ_MSG(test,
			    xfs_holetest_verify(test, va, npages, tid, what), 0,
			    "%s: error(s) detected", what);
	vm_munmap(va, sz);

	if (private) {
		/* check that no writes propagated into the original file */
		va = kunit_vm_mmap(test, f, 0, sz, PROT_READ, MAP_PRIVATE, 0);
		KUNIT_ASSERT_NE_MSG(test, va, 0UL, "%s: mmap()", what);
		KUNIT_EXPECT_EQ_MSG(test,
				    xfs_holetest_verify(test, va, npages, zero,
							what), 0,
				    "%s: private writes reached the file", what);
		vm_munmap(va, sz);
	}
}

/* not upstream: after close, the server's copy holds what was expected */
static void xfs_holetest_server(struct kunit *test, const char *server,
				loff_t sz, unsigned int flags, const char *what)
{
	bool private = flags & XFS_HOLETEST_PRIVATE;
	u64 v, want;
	loff_t p;
	int i, errs = 0;

	for (p = 0; p < sz && errs < 3; p += PAGE_SIZE) {
		for (i = 0; i < XFS_HOLETEST_THREADS; i++) {
			loff_t off = p + (PAGE_SIZE / 4) * (2 * i + 1);

			want = private ? 0 : 0x4f4c4500000000ULL | (i + 1);
			if (xfs_read_range(server, &v, sizeof(v), off) !=
			    sizeof(v) || v != want) {
				KUNIT_FAIL(test,
					   "%s: server offset %08llx holds %08llx, expected %08llx",
					   what, off, v, want);
				errs++;
			}
		}
	}
}

void xfs_holetest(struct kunit *test, const char *name, loff_t sz,
		  unsigned int flags)
{
	static const char * const tests[] = {
		"zero-filled", "posix_fallocate", "ftruncate",
	};
	char *path, *server, *what;
	struct file *f;
	int k, err;

	path = kunit_kzalloc(test, 3 * 128, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, path);
	server = path + 128;
	what = path + 256;
	snprintf(path, 128, XFS_MNT "/%s", name);
	snprintf(server, 128, XFS_EXPORT "/%s", name);

	for (k = 0; k < ARRAY_SIZE(tests); k++) {
		snprintf(what, 128, "%s test, sz = %lld", tests[k], sz);
		xfs_unlink(path);
		f = filp_open(path, O_RDWR | O_EXCL | O_CREAT | O_LARGEFILE,
			      0644);
		KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "%s: open: %ld", what,
				       PTR_ERR(f));
		if (k == 1) {
			err = vfs_fallocate(f, 0, 0, sz);
		} else {
			err = xfs_ftruncate(f, sz);
			if (!err && k == 0) {
				/* explicitly zero-fill through a mapping */
				unsigned long va, off;

				va = kunit_vm_mmap(test, f, 0, sz,
						   PROT_READ | PROT_WRITE,
						   MAP_SHARED, 0);
				KUNIT_ASSERT_NE(test, va, 0UL);
				for (off = 0; off < sz; off += PAGE_SIZE)
					if (clear_user((void __user *)(va + off),
						       PAGE_SIZE))
						err = -EFAULT;
				vm_munmap(va, sz);
			}
		}
		KUNIT_ASSERT_EQ_MSG(test, err, 0, "%s: sizing the file", what);

		xfs_holetest_this(test, f, sz, flags, what);
		filp_close(f, NULL);
		xfs_holetest_server(test, server, sz, flags, what);
		/*
		 * unlink(), and -- unlike a local filesystem -- wait for the
		 * space to come back: the delayed fput of the file closed
		 * here would otherwise turn the REMOVE into a sillyrename
		 * that keeps sz allocated through the next sizing.
		 */
		xfs_settle_fput();
		KUNIT_EXPECT_EQ_MSG(test, xfs_unlink(path), 0, "%s: unlink()",
				    what);
		KUNIT_EXPECT_EQ_MSG(test, xfs_wait_for_free_bytes(sz), 0,
				    "%s: the space never came back", what);
	}
	kunit_kfree(test, path);
}

/*
 * src/mmap-rw-fault [-2] <file>: five times over, a two-page file whose
 * first page is a hole and whose second holds one byte value, written
 * with O_DIRECT, is reopened (buffered or O_DIRECT) and mapped
 * MAP_PRIVATE, and one I/O is done whose user buffer is in that mapping,
 * so the kernel faults a page of the file it is doing I/O on:
 *
 *	'a' buffered pread of page 1 into mapped page 0
 *	'b' the same with O_DIRECT
 *	'c' buffered pwrite of mapped page 1 to offset 0
 *	'd' the same with O_DIRECT
 *	'e' O_DIRECT pread of the hole at offset 0 into mapped page 0
 *	'f' (-2 only) O_DIRECT pwrite of mapped page 1 onto the offset it maps
 *
 * Each I/O must move a whole page, and after the first five the mapped
 * page 0 must hold what was read or written. Beyond upstream, 'f' checks
 * the server's copy of page 1 still holds 'f' afterwards.
 */
static struct file *xfs_mrf_init(struct kunit *test, const char *path,
				 u8 c, int flags, unsigned long *addr, u8 *page)
{
	struct file *f;
	loff_t pos = PAGE_SIZE;

	xfs_unlink(path);
	memset(page, c, PAGE_SIZE);
	f = filp_open(path, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0666);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "create: %ld", PTR_ERR(f));
	KUNIT_ASSERT_EQ(test, xfs_direct_write(f, page, PAGE_SIZE, &pos),
			(ssize_t)PAGE_SIZE);
	filp_close(f, NULL);
	f = filp_open(path, flags, 0);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(f), "reopen: %ld", PTR_ERR(f));
	*addr = kunit_vm_mmap(test, f, 0, 2 * PAGE_SIZE,
			      PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, *addr, 0UL, "mmap failed");
	return f;
}

static void xfs_mrf_done(struct kunit *test, struct file *f,
			 unsigned long addr)
{
	KUNIT_EXPECT_EQ(test, vfs_fsync(f, 0), 0);
	vm_munmap(addr, 2 * PAGE_SIZE);
	filp_close(f, NULL);
}

static void xfs_mrf_check(struct kunit *test, unsigned long addr,
			  const u8 *want, u8 *scratch, const char *what)
{
	KUNIT_ASSERT_EQ_MSG(test,
			    copy_from_user(scratch, (void __user *)addr,
					   PAGE_SIZE), 0UL,
			    "%s: reading the mapping back", what);
	KUNIT_EXPECT_EQ_MSG(test, memcmp(scratch, want, PAGE_SIZE), 0,
			    "%s is broken", what);
}

void xfs_mmap_rw_fault(struct kunit *test, const char *name, bool opt_2)
{
	static const struct {
		u8 c;
		bool direct, write;
		loff_t pos;
		const char *what;
	} cases[] = {
		{ 'a', false, false, PAGE_SIZE, "pread" },
		{ 'b', true,  false, PAGE_SIZE, "pread (O_DIRECT)" },
		{ 'c', false, true,  0, "pwrite" },
		{ 'd', true,  true,  0, "pwrite (O_DIRECT)" },
		{ 'e', true,  false, 0, "pread (O_DIRECT) from hole" },
	};
	char path[128], server[128];
	unsigned long addr;
	struct file *f;
	u8 *page, *scratch;
	loff_t pos;
	ssize_t n;
	int i;

	snprintf(path, sizeof(path), XFS_MNT "/%s", name);
	snprintf(server, sizeof(server), XFS_EXPORT "/%s", name);
	page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	scratch = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, scratch);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		f = xfs_mrf_init(test, path, cases[i].c,
				 O_RDWR | (cases[i].direct ? O_DIRECT : 0),
				 &addr, page);
		pos = cases[i].pos;
		/* a write sources mapped page 1, a read fills mapped page 0 */
		n = xfs_user_rw(f, (void __user *)(addr +
			(cases[i].write ? PAGE_SIZE : 0)), PAGE_SIZE, &pos,
				cases[i].write, cases[i].direct);
		KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE, "%s: %zd",
				    cases[i].what, n);
		if (cases[i].c == 'e')
			memset(page, 0, PAGE_SIZE);
		xfs_mrf_check(test, addr, page, scratch, cases[i].what);
		xfs_mrf_done(test, f, addr);
	}

	if (opt_2) {
		f = xfs_mrf_init(test, path, 'f', O_RDWR | O_DIRECT, &addr, page);
		pos = PAGE_SIZE;
		n = xfs_user_rw(f, (void __user *)(addr + PAGE_SIZE), PAGE_SIZE,
				&pos, true, true);
		KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)PAGE_SIZE,
				    "pwrite (O_DIRECT) onto itself: %zd", n);
		xfs_mrf_done(test, f, addr);
		KUNIT_ASSERT_EQ(test,
				xfs_read_range(server, scratch, PAGE_SIZE,
					       PAGE_SIZE), (ssize_t)PAGE_SIZE);
		KUNIT_EXPECT_TRUE_MSG(test, !memchr_inv(scratch, 'f', PAGE_SIZE),
				      "the page written onto itself changed");
	}

	/* if (unlink(filename)) err(...) */
	xfs_settle_fput();
	KUNIT_EXPECT_EQ(test, xfs_unlink(path), 0);
	kunit_kfree(test, scratch);
	kunit_kfree(test, page);
}

int xfs_posix_lock(struct file *f, unsigned char type, loff_t start,
		   loff_t end, fl_owner_t owner, bool wait)
{
	struct file_lock *fl;
	int err;

	fl = locks_alloc_lock();
	if (!fl)
		return -ENOMEM;
	fl->c.flc_type = type;
	fl->c.flc_flags = FL_POSIX | (wait ? FL_SLEEP : 0);
	fl->c.flc_owner = owner;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_file = f;
	fl->fl_start = start;
	fl->fl_end = end;

	err = vfs_lock_file(f, wait ? F_SETLKW : F_SETLK, fl, NULL);
	locks_free_lock(fl);
	return err;
}

/*
 * ---------------------------------------------------------------------
 * Bring-up / teardown
 * ---------------------------------------------------------------------
 */

static DEFINE_MUTEX(xfs_fixture_lock);
static int xfs_fixture_refs;
static char xfs_export_mount_opts[64] = XFS_EXPORT_OPTS_DEFAULT;
/* The bring-up thread's root, captured by xfs_bringup(); see
 * xfstests_nfs_case_init(). */
static struct path xfs_bringup_root;

void xfstests_nfs_export_opts(const char *opts)
{
	strscpy(xfs_export_mount_opts, opts, sizeof(xfs_export_mount_opts));
}

static struct {
	bool	tmpfs_mounted;
	bool	nfsdfs_mounted;
	bool	nfsd_up;
	bool	client_mounted;
} xfs_env;

bool xfstests_nfs_mounted(void)
{
	return xfs_env.client_mounted;
}

static int xfs_loopback_up(void)
{
	struct net_device *lo = init_net.loopback_dev;
	int err = 0;

	rtnl_lock();
	if (!(lo->flags & IFF_UP))
		err = dev_change_flags(lo, lo->flags | IFF_UP, NULL);
	rtnl_unlock();
	return err;
}

static int xfs_mount_at(const char *dev, const char *mountpoint,
			const char *type, const char *opts)
{
	struct path p;
	char *data = NULL;
	int err;

	if (opts) {
		/* the monolithic option parser strsep()s the buffer */
		data = kstrdup(opts, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
	}
	err = kern_path(mountpoint, 0, &p);
	if (!err) {
		err = path_mount(dev, &p, type, 0, data);
		path_put(&p);
	}
	kfree(data);
	return err;
}

static int xfs_umount(const char *mountpoint)
{
	struct path p;
	int err;

	err = kern_path(mountpoint, 0, &p);
	if (err)
		return err;
	return path_umount(&p, 0);
}

/*
 * Settle the delayed fputs this thread's filp_close()s left behind.
 *
 * fput() from a kernel thread defers the final release of a struct file to
 * a workqueue, and NFS sillyrenames an unlink whose inode still has a live
 * struct file: the REMOVE becomes a RENAME to .nfsXXXX, and the name only
 * disappears when the fput lands. A test that unlinks a file it wrote and
 * then cares what the directory contains has to settle first, or it races
 * that transient entry.
 */
void xfs_settle_fput(void)
{
	flush_delayed_fput();
}

int xfs_rmdir_settled(const char *path)
{
	int err = -ENOTEMPTY;
	int tries;

	for (tries = 0; tries < 20 && err == -ENOTEMPTY; tries++) {
		flush_delayed_fput();
		err = xfs_rmdir(path);
		if (err == -ENOTEMPTY)
			msleep(100);
	}
	return err;
}

/*
 * fput() from a kernel thread defers the final release of a struct file
 * (and its pin on the mount) to the delayed-fput workqueue, so a mount can
 * look busy for a moment after the last filp_close(). Flush, and give any
 * other stragglers (async writeback completion) a bounded grace period.
 */
static int xfs_umount_settled(const char *mountpoint)
{
	int err = -EBUSY;
	int tries;

	for (tries = 0; tries < 20 && err == -EBUSY; tries++) {
		flush_delayed_fput();
		err = xfs_umount(mountpoint);
		if (err == -EBUSY)
			msleep(100);
	}
	if (err == -EBUSY) {
		/*
		 * A test case that aborted mid-assertion leaks its open
		 * struct file, and that reference never goes away. Lazily
		 * detach so the mountpoint is reusable and the next suite
		 * is not poisoned; the superblock lingers until the leaked
		 * file dies with the kernel.
		 */
		struct path p;

		if (!kern_path(mountpoint, 0, &p)) {
			err = path_umount(&p, MNT_DETACH);
			pr_warn("xfstests-nfs: lazy-detached %s (leaked file?): %d\n",
				mountpoint, err);
		}
	}
	return err;
}

/*
 * Space freed by REMOVE comes back once the server side lets go of the
 * file (knfsd's file cache holds recently used files briefly), so "the
 * space is back" is an eventually-true statement over NFS. Poll for it.
 */
int xfs_wait_for_free_bytes(u64 bytes)
{
	struct kstatfs st;
	int tries, err;

	for (tries = 0; tries < 100; tries++) {
		flush_delayed_fput();
		err = xfs_statfs(XFS_MNT, &st);
		if (err)
			return err;
		/*
		 * Careful with units: NFS reports f_bsize as the server's
		 * preferred transfer size (128K here), not 4K.
		 */
		if ((u64)st.f_bavail * st.f_bsize >= bytes)
			return 0;
		msleep(100);
	}
	pr_warn("xfstests-nfs: free-space wait: %llu bytes of %llu wanted\n",
		(u64)st.f_bavail * st.f_bsize, bytes);
	return -ETIMEDOUT;
}

/*
 * Feed one line into a sunrpc cache exactly as rpc.mountd would write it.
 * The parse functions scribble on the buffer and require a trailing
 * newline, hence the writable copy.
 */
static int xfs_cache_line(int (*parse)(struct cache_detail *, char *, int),
			  struct cache_detail *cd, const char *fmt, ...)
{
	va_list args;
	char *line;
	int err;

	va_start(args, fmt);
	line = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);
	if (!line)
		return -ENOMEM;

	err = parse(cd, line, strlen(line));
	kfree(line);
	return err;
}

static int xfs_configure_exports(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	time64_t expiry = ktime_get_real_seconds() + 3600;
	int err;

	/*
	 * Order matters: ip_map_parse() creates the auth domain
	 * (unix_domain_find); the export parsers only look it up.
	 */
	err = xfs_cache_line(ip_map_parse, sn->ip_map_cache,
			     "nfsd 127.0.0.1 %lld " XFS_DOMAIN "\n",
			     (long long)expiry);
	if (err)
		return err;

	/* client path expiry flags anonuid anongid fsid */
	err = xfs_cache_line(svc_export_parse, nn->svc_export_cache,
			     XFS_DOMAIN " " XFS_EXPORT
			     " %lld %d 65534 65534 0\n",
			     (long long)expiry, XFS_EXPFLAGS);
	if (err)
		return err;

	/* client fsidtype fsid expiry path; fsid 0 as raw \x-escaped bytes */
	return xfs_cache_line(expkey_parse, nn->svc_expkey_cache,
			      XFS_DOMAIN " 1 \\x00000000 %lld "
			      XFS_EXPORT "\n",
			      (long long)expiry);
}

static int xfs_start_nfsd(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	int nthreads[1] = { 1 };
	int err;

	mutex_lock(&nfsd_mutex);
	/*
	 * v4 only: v2/v3 would pull in lockd and rpcbind registration,
	 * neither of which exists here. Cleared before anything else touches
	 * nn, on purpose: nfsd_create_serv() below calls
	 * nfsd_reset_versions(), which only resets to the (v2/v3-enabled)
	 * defaults when NO version has been touched yet -- so this must run
	 * first, or it is a no-op and v3 gets registered anyway.
	 */
	nfsd_vers(nn, 2, NFSD_CLEAR);
	nfsd_vers(nn, 3, NFSD_CLEAR);
	/*
	 * Through v6.12.57, nfsd_startup_net() created the default TCP+UDP
	 * listeners on port 2049 itself (nfsd_init_socks()). That auto-create
	 * is gone on current mainline: nfsd_startup_net() now requires
	 * sv_permsocks already non-empty and fails with "no listeners
	 * configured" otherwise, so the listeners are created explicitly
	 * here instead, mirroring fs/nfsd/nfsctl.c's __write_ports_addxprt()
	 * (nfsd_create_serv() then svc_xprt_create() per transport). Safe on
	 * both: nfsd_create_serv() is idempotent, and nfsd_svc() below still
	 * finds sv_permsocks already populated on a kernel that would have
	 * created it internally anyway, so nfsd_init_socks()'s own check
	 * (where it still exists) just no-ops.
	 */
	err = nfsd_create_serv(net);
	/*
	 * svc_xprt_create() returns the local xprt port (e.g. 2049) on
	 * success, not 0 -- gated on >= 0, not on truthiness, or a
	 * successful create reads as failure here and everything past it
	 * silently never runs.
	 */
	if (err >= 0)
		err = svc_xprt_create(nn->nfsd_serv, "udp", net, PF_INET,
				      2049, SVC_SOCK_DEFAULTS, current_cred());
	if (err >= 0)
		err = svc_xprt_create(nn->nfsd_serv, "tcp", net, PF_INET,
				      2049, SVC_SOCK_DEFAULTS, current_cred());
	if (err >= 0)
		err = nfsd_svc(1, nthreads, net, current_cred(), NULL);
	mutex_unlock(&nfsd_mutex);
	if (err < 0)
		return err;

	/*
	 * The 90 second v4 grace period would stall the first OPEN. Ending
	 * it early is a supported admin action (/proc/fs/nfsd/v4_end_grace
	 * does exactly this call).
	 */
	nfsd4_end_grace(nn);
	return 0;
}

static void xfs_stop_nfsd(struct net *net)
{
	int nthreads[1] = { 0 };

	mutex_lock(&nfsd_mutex);
	nfsd_svc(1, nthreads, net, current_cred(), NULL);
	mutex_unlock(&nfsd_mutex);
}

static int xfs_mkdir_tolerant(const char *path)
{
	int err = xfs_mkdir(path);

	return (err == -EEXIST) ? 0 : err;
}

static int xfs_bringup(void)
{
	int err;

	err = xfs_loopback_up();
	if (err) {
		pr_warn("xfstests-nfs: loopback up failed: %d\n", err);
		return err;
	}

	err = xfs_mkdir_tolerant(XFS_EXPORT);
	if (err) {
		pr_warn("xfstests-nfs: mkdir " XFS_EXPORT ": %d\n", err);
		return err;
	}
	/* bare rootfs has no /mnt; parents first, mkdir is not recursive */
	err = xfs_mkdir_tolerant("/mnt");
	if (err) {
		pr_warn("xfstests-nfs: mkdir /mnt: %d\n", err);
		return err;
	}
	err = xfs_mkdir_tolerant(XFS_MNT);
	if (err) {
		pr_warn("xfstests-nfs: mkdir " XFS_MNT ": %d\n", err);
		return err;
	}
	err = xfs_mkdir_tolerant(XFS_NFSDFS);
	if (err) {
		pr_warn("xfstests-nfs: mkdir " XFS_NFSDFS ": %d\n", err);
		return err;
	}

	/* ramfs is not exportable (no export_operations); tmpfs is. */
	err = xfs_mount_at("none", XFS_EXPORT, "tmpfs", xfs_export_mount_opts);
	if (err) {
		pr_warn("xfstests-nfs: tmpfs mount failed: %d\n", err);
		return err;
	}
	xfs_env.tmpfs_mounted = true;

	err = xfs_mount_at("nfsd", XFS_NFSDFS, "nfsd", NULL);
	if (err) {
		pr_warn("xfstests-nfs: nfsdfs mount failed: %d\n", err);
		return err;
	}
	xfs_env.nfsdfs_mounted = true;

	err = xfs_configure_exports(&init_net);
	if (err) {
		pr_warn("xfstests-nfs: export setup failed: %d\n", err);
		return err;
	}

	err = xfs_start_nfsd(&init_net);
	if (err) {
		pr_warn("xfstests-nfs: nfsd start failed: %d\n", err);
		return err;
	}
	xfs_env.nfsd_up = true;

	err = xfs_mount_at("127.0.0.1:/", XFS_MNT, "nfs4",
			   "addr=127.0.0.1,clientaddr=127.0.0.1,vers=4.2,sec=sys");
	if (err) {
		pr_warn("xfstests-nfs: NFS client mount failed: %d\n", err);
		return err;
	}
	xfs_env.client_mounted = true;

	/*
	 * KUnit runs each test case's body in its own fresh kthread
	 * (lib/kunit/try_catch.c), spawned fresh per case and *not* sharing
	 * this thread's fs_struct/root. On current mainline that kthread's
	 * root never saw the mkdir/mount calls above, so the very first path
	 * lookup a test makes under XFS_MNT fails with ENOENT before ever
	 * reaching NFS code -- see docs/kunit-nfs-reference.md. Every suite wires
	 * xfstests_nfs_case_init() in as .init (run-nfs-kunit.sh adds it
	 * automatically) so each fresh test-case thread rebinds to this
	 * thread's root before the test body runs.
	 */
	if (xfs_bringup_root.dentry)
		path_put(&xfs_bringup_root);
	xfs_bringup_root = current->fs->root;
	path_get(&xfs_bringup_root);

	return 0;
}

int xfstests_nfs_case_init(struct kunit *test)
{
	if (xfs_bringup_root.dentry) {
		set_fs_root(current->fs, &xfs_bringup_root);
		set_fs_pwd(current->fs, &xfs_bringup_root);
	}
	return 0;
}

static void xfs_teardown(void)
{
	int err;

	if (xfs_env.client_mounted) {
		err = xfs_umount_settled(XFS_MNT);
		if (err)
			pr_err("xfstests-nfs: client umount failed: %d\n", err);
		xfs_env.client_mounted = false;
	}
	if (xfs_env.nfsd_up) {
		xfs_stop_nfsd(&init_net);
		xfs_env.nfsd_up = false;
	}
	if (xfs_env.nfsdfs_mounted) {
		err = xfs_umount(XFS_NFSDFS);
		if (err)
			pr_err("xfstests-nfs: nfsdfs umount failed: %d\n", err);
		xfs_env.nfsdfs_mounted = false;
	}
	if (xfs_env.tmpfs_mounted) {
		err = xfs_umount(XFS_EXPORT);
		if (err)
			pr_err("xfstests-nfs: tmpfs umount failed: %d\n", err);
		xfs_env.tmpfs_mounted = false;
	}
	xfs_rmdir(XFS_MNT);
	xfs_rmdir(XFS_NFSDFS);
	xfs_rmdir(XFS_EXPORT);
	/* a suite-specific export size applies to one bring-up only */
	strscpy(xfs_export_mount_opts, XFS_EXPORT_OPTS_DEFAULT,
		sizeof(xfs_export_mount_opts));
	if (xfs_bringup_root.dentry) {
		path_put(&xfs_bringup_root);
		xfs_bringup_root = (struct path) { };
	}
}

int xfs_remount_client(bool ro)
{
	struct path p;
	int err = -EBUSY;
	int tries;

	/*
	 * Going read-only requires no write references on the mount, and a
	 * just-closed file's delayed fput still holds one. Same settling
	 * dance as the unmount.
	 */
	for (tries = 0; tries < 20 && err == -EBUSY; tries++) {
		flush_delayed_fput();
		err = kern_path(XFS_MNT, 0, &p);
		if (err)
			return err;
		err = path_mount("127.0.0.1:/", &p, "nfs4",
				 MS_REMOUNT | (ro ? MS_RDONLY : 0), NULL);
		path_put(&p);
		if (err == -EBUSY)
			msleep(100);
	}
	return err;
}

int xfstests_nfs_get(void)
{
	int err = 0;

	mutex_lock(&xfs_fixture_lock);
	if (xfs_fixture_refs == 0)
		err = xfs_bringup();
	if (!err)
		xfs_fixture_refs++;
	else
		xfs_teardown();	/* clean up a half-built stack */
	mutex_unlock(&xfs_fixture_lock);
	return err;
}

void xfstests_nfs_put(void)
{
	mutex_lock(&xfs_fixture_lock);
	if (!WARN_ON(xfs_fixture_refs <= 0) && --xfs_fixture_refs == 0)
		xfs_teardown();
	mutex_unlock(&xfs_fixture_lock);
}

MODULE_DESCRIPTION("Loopback NFS fixture for the xfstests KUnit ports");
MODULE_LICENSE("GPL");
