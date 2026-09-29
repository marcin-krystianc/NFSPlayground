#!/bin/bash
# Run upstream xfstests against NFS inside a User Mode Linux kernel built from
# ./linux. The NFS client and knfsd under test are that tree's, so any kernel
# ref fetch-sources.sh can check out is testable, unlike
# 00-run-xfstests-on-gh-ci.sh, which is stuck with the host's kernel.
#
# The guest's root filesystem is the host's own, over hostfs, so xfstests,
# xfsprogs and nfs-utils are whatever the host has installed and nothing is
# copied into an image. scripts/xfstests-uml-init.sh is the guest's PID 1: it
# mounts tmpfs over every directory it writes, so the host's /etc, /run and
# /var/lib/nfs are never changed. The two NFS exports are xfs on UML block
# devices backed by sparse files in RUN_DIR.
#
# xfstests runs as a child of the guest's init, in the initial pid namespace,
# so /proc/locks and friends behave as on bare metal (see generic/504).
#
# UML runs as the invoking user, not root. hostfs writes go through that
# user's permissions, which is what keeps guest root from modifying the host.
#
# Usage: bash scripts/00-run-xfstests-in-uml.sh
#        bash scripts/00-run-xfstests-in-uml.sh generic/001 generic/504
#        bash scripts/00-run-xfstests-in-uml.sh -g quick
#
# Exits with xfstests' own status.

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
LINUX_DIR="${LINUX_DIR:-${REPO_ROOT}/linux}"
XFSTESTS_DIR="${XFSTESTS_DIR:-${REPO_ROOT}/xfstests}"
BUILD_DIR="${BUILD_DIR:-${LINUX_DIR}/.xfstests-uml}"
RUN_DIR="${RUN_DIR:-${REPO_ROOT}/uml-run}"

UML_MEM="${UML_MEM:-2G}"
# Sparse, so this is a ceiling. generic/103 fills one to it.
NFS_IMG_SIZE="${NFS_IMG_SIZE:-8G}"
NFS_VERS="${NFS_VERS:-4.2}"
# 0 turns off fs.leases-enable in the guest, which disables knfsd
# delegations. The sysctl is the guest kernel's, so nothing needs restoring.
NFS_DELEGATIONS="${NFS_DELEGATIONS:-1}"
# Space-separated tracefs events ("system:event") to record in the guest,
# for example "nfs4:nfs4_set_delegation nfsd:nfsd_vfs_setattr". Empty means
# no tracing and no tracing in the kernel config. The ring buffer overwrites,
# so RUN_DIR/trace.txt holds the last TRACE_BUFFER_KB of events.
TRACE_EVENTS="${TRACE_EVENTS:-}"
TRACE_BUFFER_KB="${TRACE_BUFFER_KB:-65536}"
# 1 builds the kernel with UML's gcov support and, after the run, writes
# COVERAGE_DIR/coverage.info and an HTML report, as COVERAGE=1
# scripts/kunit/run-nfs-kunit.sh does. UML is an ordinary host process, so
# the .gcda files land in BUILD_DIR when it exits.
COVERAGE="${COVERAGE:-0}"
COVERAGE_DIR="${COVERAGE_DIR:-${REPO_ROOT}/coverage/xfstests-uml}"

# Same default as 00-run-xfstests-on-gh-ci.sh.
DEFAULT_CHECK_ARGS=(-g attr -g acl -g dir)
# Shared with 00-run-xfstests-on-gh-ci.sh; see the file for the rules.
EXCLUDE_FILE="${EXCLUDE_FILE:-${REPO_ROOT}/scripts/xfstests-exclude}"

# Everything the guest needs, built in. UML loads no modules here: the guest
# sees the host's /lib/modules, which belongs to a different kernel.
KCONFIG_ENABLE=(
    HOSTFS BLK_DEV BLK_DEV_UBD BLK_DEV_LOOP
    XFS_FS XFS_POSIX_ACL
    NETWORK_FILESYSTEMS NFS_FS NFS_V4 NFS_V4_1 NFS_V4_2 NFS_SWAP
    NFSD NFSD_V4
    FILE_LOCKING FS_POSIX_ACL TMPFS TMPFS_POSIX_ACL TMPFS_XATTR
    DEVTMPFS PROC_FS SYSFS MAGIC_SYSRQ
    NET INET IPV6 UNIX
    SWAP AIO IO_URING FHANDLE INOTIFY_USER FANOTIFY
)
[ -z "$TRACE_EVENTS" ] || KCONFIG_ENABLE+=(FTRACE ENABLE_DEFAULT_TRACERS EVENT_TRACING)
# UML's own GCOV symbol (arch/um/Kconfig.debug), which needs debug info.
[ "$COVERAGE" != 1 ] ||
    KCONFIG_ENABLE+=(DEBUG_KERNEL DEBUG_INFO DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT GCOV)

log()  { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

check_args=("$@")
[ ${#check_args[@]} -gt 0 ] || check_args=("${DEFAULT_CHECK_ARGS[@]}")
# The guest sees the host's filesystem, so the host path works in there.
if [ -s "$EXCLUDE_FILE" ] && grep -qvE '^\s*(#|$)' "$EXCLUDE_FILE"; then
    check_args=(-E "$EXCLUDE_FILE" "${check_args[@]}")
fi

[ -f "${LINUX_DIR}/init/main.c" ] ||
    die "${LINUX_DIR} is not a kernel tree -- run: scripts/fetch-sources.sh linux"
[ -f "${XFSTESTS_DIR}/check" ] ||
    die "no xfstests at ${XFSTESTS_DIR} -- git submodule update --init xfstests"
for cmd in mkfs.xfs rpc.nfsd rpc.mountd exportfs rpcbind ip; do
    command -v "$cmd" >/dev/null ||
        die "$cmd not found -- the guest runs the host's userspace, so install it here"
done
if [ "$COVERAGE" = 1 ]; then
    command -v lcov >/dev/null && command -v genhtml >/dev/null ||
        die "COVERAGE=1 needs lcov and genhtml (apt install lcov)"
    # UML's linker scripts keep only the plain .fini_array section, so gcov's
    # exit destructor, emitted into .fini_array.NNNNN, is dropped and no
    # .gcda is ever written. Same fix as scripts/kunit/run-nfs-kunit.sh,
    # which has the background.
    for lds in "${LINUX_DIR}/arch/um/include/asm/common.lds.S" \
               "${LINUX_DIR}/arch/um/kernel/dyn.lds.S"; do
        grep -qF '*(.fini_array.*)' "$lds" && continue
        sed -i 's/\*(\.fini_array)/*(.fini_array.*) *(.fini_array)/' "$lds"
        grep -qF '*(.fini_array.*)' "$lds" ||
            die "could not patch .fini_array in ${lds}"
    done
fi

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------
log "configuring the UML kernel in ${BUILD_DIR}"
make -C "$LINUX_DIR" O="$BUILD_DIR" ARCH=um defconfig >/dev/null
enable_args=()
for sym in "${KCONFIG_ENABLE[@]}"; do enable_args+=(--enable "$sym"); done
# MODULES off: nothing is loaded (see KCONFIG_ENABLE), and UML's GCOV
# depends on !MODULES.
"${LINUX_DIR}/scripts/config" --file "${BUILD_DIR}/.config" "${enable_args[@]}" \
    --disable MODULES
make -C "$LINUX_DIR" O="$BUILD_DIR" ARCH=um olddefconfig >/dev/null

# olddefconfig silently drops a symbol whose dependencies are unmet. Say
# which, rather than boot a guest that cannot mount NFS. A symbol the tree
# does not define at all is fine: the list spans kernel versions, and for
# example NFS_V4_1 is gone on master, where v4.1 is always built.
missing=()
for sym in "${KCONFIG_ENABLE[@]}"; do
    grep -qx "CONFIG_${sym}=y" "${BUILD_DIR}/.config" && continue
    if grep -rqx --include='Kconfig*' "config ${sym}" "$LINUX_DIR"; then
        missing+=("$sym")
    else
        echo "  ${sym}: not defined in this tree, skipped"
    fi
done
[ ${#missing[@]} -eq 0 ] ||
    die "not enabled after olddefconfig: ${missing[*]}"

log "building the UML kernel"
make -C "$LINUX_DIR" O="$BUILD_DIR" ARCH=um -j"$(nproc)" >"${BUILD_DIR}/build.log" 2>&1 ||
    die "kernel build failed -- see ${BUILD_DIR}/build.log"

# ---------------------------------------------------------------------------
# xfstests, built on the host because the guest runs the host's binaries
# ---------------------------------------------------------------------------
if [ -x "${XFSTESTS_DIR}/ltp/fsstress" ]; then
    log "xfstests already built"
else
    log "building xfstests"
    make -C "$XFSTESTS_DIR" -j"$(nproc)" >/tmp/xfstests-build.log 2>&1 ||
        die "xfstests build failed -- see /tmp/xfstests-build.log"
fi

# ---------------------------------------------------------------------------
# Run directory: everything the guest reads, and where it reports back
# ---------------------------------------------------------------------------
log "preparing ${RUN_DIR}"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR"
for name in test scratch; do
    truncate -s "$NFS_IMG_SIZE" "${RUN_DIR}/${name}.img"
done

# NFSv4 paths are relative to the fsid=0 pseudo-root the guest exports.
cat > "${RUN_DIR}/local.config" <<EOF
# generated by 00-run-xfstests-in-uml.sh -- do not edit, it is overwritten
export FSTYP=nfs
export TEST_DEV=127.0.0.1:/test
export TEST_DIR=/mnt/nfs-test-env/test
export SCRATCH_DEV=127.0.0.1:/scratch
export SCRATCH_MNT=/mnt/nfs-test-env/scratch
export NFS_MOUNT_OPTIONS="-o vers=${NFS_VERS}"
EOF

cat > "${RUN_DIR}/env" <<EOF
XFSTESTS_DIR='${XFSTESTS_DIR}'
NFS_VERS='${NFS_VERS}'
NFS_DELEGATIONS='${NFS_DELEGATIONS}'
TRACE_EVENTS='${TRACE_EVENTS}'
TRACE_BUFFER_KB='${TRACE_BUFFER_KB}'
EOF
printf '%s\n' "${check_args[@]}" > "${RUN_DIR}/check-args"

log "under test"
echo "  kernel:       $(make -s -C "$LINUX_DIR" O="$BUILD_DIR" ARCH=um kernelrelease)"
echo "  source:       $(git -C "$LINUX_DIR" describe --always --dirty 2>/dev/null || echo unknown)"
echo "  nfs-utils:    $(dpkg-query -W -f='${Version}' nfs-common 2>/dev/null || echo unknown)"
echo "  delegations:  $([ "$NFS_DELEGATIONS" = 1 ] && echo on || echo off)"
echo "  trace events: ${TRACE_EVENTS:-(none)}"
echo "  coverage:     $([ "$COVERAGE" = 1 ] && echo "on (${COVERAGE_DIR})" || echo off)"
echo "  check args:   ${check_args[*]}"

# ---------------------------------------------------------------------------
# Boot. Arguments after "--" go to init. init is bash with the script as its
# argument, so the script needs no exec bit, which git does not record here.
# The guest powers itself off when check finishes; the status file is how
# its result gets out.
# ---------------------------------------------------------------------------
# Counters from an earlier run would otherwise be added to this one's.
[ "$COVERAGE" != 1 ] || find "$BUILD_DIR" -name '*.gcda' -delete

log "booting the guest"
"${BUILD_DIR}/linux" \
    mem="$UML_MEM" \
    rootfstype=hostfs rootflags=/ rw \
    ubd0="${RUN_DIR}/test.img" ubd1="${RUN_DIR}/scratch.img" \
    con0=fd:0,fd:1 con=null ssl=null \
    quiet \
    init=/bin/bash \
    -- "${REPO_ROOT}/scripts/xfstests-uml-init.sh" "$RUN_DIR" </dev/null || true

rm -f "${RUN_DIR}/test.img" "${RUN_DIR}/scratch.img"

if [ "$COVERAGE" = 1 ]; then
    log "collecting coverage into ${COVERAGE_DIR}"
    mkdir -p "$COVERAGE_DIR"
    # --ignore-errors mismatch: see the same call in run-nfs-kunit.sh.
    # geninfo warns per source line; that goes to lcov.log, not the console.
    lcov -q -t xfstests-uml -o "${COVERAGE_DIR}/coverage.info" -c -d "$BUILD_DIR" \
        --ignore-errors mismatch 2>"${COVERAGE_DIR}/lcov.log" ||
        die "lcov found no coverage -- see ${COVERAGE_DIR}/lcov.log"
    genhtml -q -o "${COVERAGE_DIR}/html" "${COVERAGE_DIR}/coverage.info" \
        2>>"${COVERAGE_DIR}/lcov.log"
    lcov --summary "${COVERAGE_DIR}/coverage.info" 2>&1 | sed 's/^/  /'
fi

[ -f "${RUN_DIR}/status" ] ||
    die "the guest exited without a result -- see the console output above and ${RUN_DIR}/dmesg.txt"
status="$(cat "${RUN_DIR}/status")"
if [ "$status" -eq 0 ]; then
    log "xfstests passed"
else
    log "xfstests failed (status $status) -- see ${XFSTESTS_DIR}/results/"
fi
exit "$status"
