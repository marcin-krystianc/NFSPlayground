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
 *    un-staticed by scripts/kunit/run-sunrpc-kunit.sh
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
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/cache.h>

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
/* fs/namei.c syscall body without a declaration in fs/internal.h */
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

int xfs_mknod_chr(const char *path)
{
	/* an arbitrary char device identity; NFSv4 CREATE type NF4CHR */
	return do_mknodat(AT_FDCWD, getname_kernel(path), S_IFCHR | 0666,
			  new_encode_dev(MKDEV(1, 3)));
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

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	got = kernel_read(f, buf, len, &off);
	filp_close(f, NULL);
	return got;
}

/*
 * ---------------------------------------------------------------------
 * Bring-up / teardown
 * ---------------------------------------------------------------------
 */

static DEFINE_MUTEX(xfs_fixture_lock);
static int xfs_fixture_refs;

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
	return err;
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
	 * neither of which exists here. nfsd_startup_net() then creates the
	 * default TCP+UDP listeners on port 2049 itself (nfsd_init_socks).
	 */
	nfsd_vers(nn, 2, NFSD_CLEAR);
	nfsd_vers(nn, 3, NFSD_CLEAR);
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
	err = xfs_mount_at("none", XFS_EXPORT, "tmpfs", NULL);
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
