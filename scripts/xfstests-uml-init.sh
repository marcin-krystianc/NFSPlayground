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
    [ -z "${TRACE_EVENTS:-}" ] ||
        cat /sys/kernel/tracing/trace > "${RUN_DIR}/trace.txt" 2>/dev/null
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

# The users and groups xfstests' _require_user and _require_group want,
# defined here whether or not the host has them, in tmpfs copies of passwd,
# group and shadow bind-mounted over the host's:
#  - A host user's home is on hostfs, which the unprivileged host user
#    running UML cannot enter, so su warns and the test output differs.
#    Home is /tmp here.
#  - su goes through PAM, whose unix_chkpwd reads /etc/shadow. The host's is
#    root-only, so no host entry is readable. Root needs no password to su,
#    so "*" will do.
qa_users=(fsgqa fsgqa2 123456-fsgqa)
for f in passwd group; do
    grep -vE "^($(IFS='|'; echo "${qa_users[*]}")):" "/etc/${f}" > "/run/${f}"
done
: > /run/shadow
id=1100
for u in "${qa_users[@]}"; do
    echo "${u}:x:${id}:" >> /run/group
    echo "${u}:x:${id}:1100::/tmp:/bin/bash" >> /run/passwd
    echo "${u}:*:19000:0:99999:7:::" >> /run/shadow
    id=$((id + 1))
done
chmod 600 /run/shadow
for f in passwd group shadow; do
    mount --bind "/run/${f}" "/etc/${f}"
done

# The exports: xfs on the two UML block devices, under an fsid=0 pseudo-root.
# 00-run-xfstests-in-uml.sh writes this path into NFSv3 device names.
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
#
# nfs-utils' built-in defaults, not the host's nfs.conf: Ubuntu's sets
# manage-gids=y for mountd, which makes the server replace the client's
# supplementary groups with its own /etc/group lookup, and xfstests' ACL
# tests (generic/099) run as ids that have no entry there.
: > /run/nfs.conf
mount --bind /run/nfs.conf /etc/nfs.conf
[ -d /etc/nfs.conf.d ] && mount -t tmpfs tmpfs /etc/nfs.conf.d
mkdir -p /var/lib/nfs/rpc_pipefs /var/lib/nfs/v4recovery
touch /var/lib/nfs/etab /var/lib/nfs/rmtab
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs
mount -t nfsd nfsd /proc/fs/nfsd
rpcbind -w

# Kerberos, for NFS_SEC=krb5/krb5i/krb5p: a KDC for realm UML.TEST, one
# host name (uml.test) for client and server, and nfs/uml.test in a keytab
# that rpc.gssd uses as root's machine credential and rpc.svcgssd as the
# server's. KRB5_CONFIG and KRB5_KDC_PROFILE point at files under /run, so
# the host's /etc/krb5.conf is not used. Kerberos NFSv4 sends user names,
# so rpc.idmapd and nfsidmap need the domain, and the machine principal is
# mapped to root. The test users get tickets in /tmp/krb5cc_<uid>, where
# rpc.gssd looks for them; any other uid has no credentials.
if [ -n "$NFS_SEC" ]; then
    k=/run/krb5
    mkdir -p "$k"
    hostname uml.test
    { cat /etc/hosts; echo "127.0.0.1 uml.test"; } > /run/hosts
    mount --bind /run/hosts /etc/hosts
    export KRB5_CONFIG="${k}/krb5.conf" KRB5_KDC_PROFILE="${k}/kdc.conf"
    export KRB5_KTNAME="FILE:${k}/krb5.keytab"
    printf '%s\n' \
        '[libdefaults]' \
        '    default_realm = UML.TEST' \
        "    default_keytab_name = FILE:${k}/krb5.keytab" \
        '    dns_lookup_kdc = false' \
        '    dns_lookup_realm = false' \
        '    rdns = false' \
        '[realms]' \
        '    UML.TEST = {' \
        '        kdc = uml.test' \
        '    }' \
        '[domain_realm]' \
        '    uml.test = UML.TEST' > "$KRB5_CONFIG"
    printf '%s\n' \
        '[realms]' \
        '    UML.TEST = {' \
        "        database_name = ${k}/principal" \
        "        key_stash_file = ${k}/stash" \
        '    }' > "$KRB5_KDC_PROFILE"
    kdb5_util create -s -r UML.TEST -P "$(head -c 24 /dev/urandom | base64)" >/dev/null
    for p in nfs/uml.test "${qa_users[@]}"; do
        kadmin.local -q "addprinc -randkey ${p}" >/dev/null
    done
    kadmin.local -q "ktadd -k ${k}/krb5.keytab nfs/uml.test" >/dev/null
    krb5kdc
    printf '%s\n' \
        '[General]' \
        'Domain = uml.test' \
        'Local-Realms = UML.TEST' \
        '[Mapping]' \
        'Nobody-User = nobody' \
        "Nobody-Group = $(getent group nogroup >/dev/null && echo nogroup || echo nobody)" \
        '[Translation]' \
        'Method = nsswitch' \
        'GSS-Methods = static,nsswitch' \
        '[Static]' \
        'nfs/uml.test@UML.TEST = root' > /run/idmapd.conf
    mount --bind /run/idmapd.conf /etc/idmapd.conf
    for u in "${qa_users[@]}"; do
        kadmin.local -q "ktadd -k ${k}/${u}.keytab ${u}" >/dev/null
        cc="/tmp/krb5cc_$(id -u "$u")"
        KRB5CCNAME="FILE:${cc}" kinit -k -t "${k}/${u}.keytab" "$u"
        chown "$u" "$cc"
    done
    rpc.idmapd -p /var/lib/nfs/rpc_pipefs
    rpc.svcgssd
    # rpc.gssd reads /etc/krb5.keytab unless told otherwise.
    rpc.gssd -p /var/lib/nfs/rpc_pipefs -k "${k}/krb5.keytab"
fi
# NFSv3 locking needs statd on both ends of lockd; mount.nfs would try to
# start it through systemd, which the guest does not run.
case "$NFS_VERS" in
    3*) mkdir -p /var/lib/nfs/sm /var/lib/nfs/sm.bak && rpc.statd ;;
esac
# No client has state to reclaim in a fresh guest. 10 s is the shortest
# grace period nfsd accepts, and it can only be set before nfsd starts;
# v4_end_grace cannot end it early here, since nfsd4_force_end_grace()
# refuses without client tracking and the guest runs no nfsdcld. The lease
# time is left at its default, so test timing is unchanged.
echo 10 > /proc/fs/nfsd/nfsv4gracetime
sec="${NFS_SEC:+,sec=${NFS_SEC}}"
exportfs -i -o "ro,sync,no_subtree_check,no_root_squash,fsid=0${sec}" "127.0.0.1:${base}"
exportfs -i -o "rw,sync,no_subtree_check,no_root_squash,fsid=1${sec}" "127.0.0.1:${base}/test"
exportfs -i -o "rw,sync,no_subtree_check,no_root_squash,fsid=2${sec}" "127.0.0.1:${base}/scratch"
rpc.mountd
rpc.nfsd 8
exportfs -v

mkdir -p /mnt/nfs-test-env/test /mnt/nfs-test-env/scratch
mount -t nfs -o "$NFS_OPTS" "${NFS_SERVER}:${NFS_ROOT}/test" /mnt/nfs-test-env/test
# The options the kernel settled on, which is what the run log should show.
echo "nfs mount: $(awk '$2 == "/mnt/nfs-test-env/test" { print $4 }' /proc/mounts)"
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

# Kernel trace events, when asked for. Enabled one by one so a name this
# kernel does not have is reported and skipped rather than failing the run.
# check writes "run fstests <test>" to /dev/kmsg; printk:console puts those
# lines in the trace too, as test boundaries.
if [ -n "$TRACE_EVENTS" ]; then
    t=/sys/kernel/tracing
    mount -t tracefs tracefs "$t"
    echo "$TRACE_BUFFER_KB" > "${t}/buffer_size_kb"
    set -f
    for ev in $TRACE_EVENTS printk:console; do
        echo "$ev" >> "${t}/set_event" 2>/dev/null ||
            echo "warning: no trace event $ev in this kernel"
    done
    set +f
    echo 1 > "${t}/tracing_on"
    echo "tracing: $(wc -l < "${t}/set_event") events, ${TRACE_BUFFER_KB} KB buffer"
fi

mapfile -t check_args < "${RUN_DIR}/check-args"
set +e
( cd "$XFSTESTS_DIR" && HOST_OPTIONS="${RUN_DIR}/local.config" ./check -nfs "${check_args[@]}" )
status=$?
