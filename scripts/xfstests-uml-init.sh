#!/bin/bash
# PID 1 of the UML guest that scripts/00-run-xfstests-in-uml.sh boots. Not
# meant to be run by hand.
#
# The root filesystem is the host's, over hostfs. Every directory written
# below gets a tmpfs first, so the host's files are only ever read. The
# exception is RUN_DIR and xfstests' results/, which are the way out.
#
# PID 1 must never exit, or the kernel panics: the EXIT trap records the
# status and powers off instead.

RUN_DIR="$1"
status=1

finish() {
    # Copy without ownership: the files are guest root's, and the host side
    # of hostfs cannot chown.
    [ -z "${results_host:-}" ] ||
        cp -r "${XFSTESTS_DIR}/results/." "$results_host"/ 2>/dev/null
    dmesg > "${RUN_DIR}/dmesg.txt" 2>/dev/null
    echo "$status" > "${RUN_DIR}/status"
    sync
    echo o > /proc/sysrq-trigger
    sleep 60
}
trap finish EXIT

set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
. "${RUN_DIR}/env"

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
# devtmpfs has no /dev/fd or /dev/std*; udev normally adds them. Bash's
# <(...) needs /dev/fd, and common/reflink's _compare_range uses it.
ln -sfn /proc/self/fd /dev/fd
ln -sf /proc/self/fd/0 /dev/stdin
ln -sf /proc/self/fd/1 /dev/stdout
ln -sf /proc/self/fd/2 /dev/stderr
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
for d in /tmp /run /var/tmp /var/lib/nfs /mnt; do
    mount -t tmpfs tmpfs "$d"
done
ip link set lo up
hostname uml-xfstests

# Users xfstests' _require_user wants. Added to a tmpfs copy of passwd and
# group, bind-mounted over the host's, only when the host lacks them.
cp /etc/passwd /etc/group /run/
grep -q '^fsgqa:' /run/group || echo 'fsgqa:x:1100:' >> /run/group
gid="$(awk -F: '$1 == "fsgqa" { print $3 }' /run/group)"
uid=1100
for u in fsgqa fsgqa2 123456-fsgqa; do
    grep -q "^${u}:" /run/passwd ||
        echo "${u}:x:$((uid++)):${gid}::/tmp:/bin/bash" >> /run/passwd
done
mount --bind /run/passwd /etc/passwd
mount --bind /run/group /etc/group

# The exports: xfs on the two UML block devices, under an fsid=0 pseudo-root.
base=/run/nfs-test-env
mkdir -p "${base}/test" "${base}/scratch"
mkfs.xfs -f -q /dev/ubda
mkfs.xfs -f -q /dev/ubdb
mount /dev/ubda "${base}/test"
mount /dev/ubdb "${base}/scratch"
chmod 777 "${base}/test" "${base}/scratch"

[ "$NFS_DELEGATIONS" = 1 ] || echo 0 > /proc/sys/fs/leases-enable

# The knfsd start sequence nfs-server.service would run. exportfs -i
# ignores /etc/exports, so the host's is neither read nor written.
mkdir -p /var/lib/nfs/rpc_pipefs /var/lib/nfs/v4recovery
touch /var/lib/nfs/etab /var/lib/nfs/rmtab
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs
mount -t nfsd nfsd /proc/fs/nfsd
rpcbind -w
# No client has state to reclaim in a fresh guest. 10 s is the shortest
# grace period nfsd accepts, and it can only be set before nfsd starts;
# v4_end_grace cannot end it early here, since nfsd4_force_end_grace()
# refuses without client tracking and the guest runs no nfsdcld. The lease
# time is left at its default, so test timing is unchanged.
echo 10 > /proc/fs/nfsd/nfsv4gracetime
exportfs -i -o ro,sync,no_subtree_check,no_root_squash,fsid=0 "127.0.0.1:${base}"
exportfs -i -o rw,sync,no_subtree_check,no_root_squash,fsid=1 "127.0.0.1:${base}/test"
exportfs -i -o rw,sync,no_subtree_check,no_root_squash,fsid=2 "127.0.0.1:${base}/scratch"
rpc.mountd
rpc.nfsd 8
exportfs -v

mkdir -p /mnt/nfs-test-env/test /mnt/nfs-test-env/scratch
mount -t nfs -o "vers=${NFS_VERS}" 127.0.0.1:/test /mnt/nfs-test-env/test
umount /mnt/nfs-test-env/test

echo "guest kernel: $(uname -r), delegations: $(cat /proc/sys/fs/leases-enable)"
# check and the tests keep temp files under ${TMPDIR:-/tmp}, which is tmpfs
# here, and check moves them into results/. Across filesystems mv copies and
# then chowns, which hostfs refuses, so results/ is tmpfs too for the run,
# at the same path, and finish() copies it back out.
results_dir="${XFSTESTS_DIR}/results"
mkdir -p "$results_dir" /run/results-host
mount --bind "$results_dir" /run/results-host
mount -t tmpfs tmpfs "$results_dir"
cp -a /run/results-host/. "$results_dir"/
results_host=/run/results-host

mapfile -t check_args < "${RUN_DIR}/check-args"
set +e
( cd "$XFSTESTS_DIR" && HOST_OPTIONS="${RUN_DIR}/local.config" ./check -nfs "${check_args[@]}" )
status=$?
