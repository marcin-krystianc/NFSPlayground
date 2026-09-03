#!/bin/bash
# Run the whole xfstests workload inside one privileged container, on a
# machine that has docker. The third sibling of
# 00-run-xfstests-on-gh-ci.sh and 00-run-xfstests-on-vm-and-docker.sh.
#
# The difference from the other two is what is containerised:
#
#   00-run-xfstests-on-gh-ci.sh          no containers; the host is everything
#   00-run-xfstests-on-vm-and-docker.sh  servers in containers, client on host
#   this one                             client AND server in ONE container
#
# This is a thin wrapper on purpose: it starts the container and runs
# 00-run-xfstests-on-gh-ci.sh inside it, so there is exactly one description
# of the setup and no third copy to keep in step. Everything below is about
# the container, not about NFS.
#
# Why bother, given the kernel is the host's either way:
#
#  1. Pinned userspace. nfs-utils, xfsprogs, xfs_io and coreutils stop being
#     "whatever the runner image ships this week", which is what made results
#     hard to compare between CI and a developer's box.
#  2. It disables xfstests' coredump harvesting, legitimately.
#     _start_coredumpctl_collection (common/rc:5113) needs coredumpctl,
#     timedatectl AND jq all present, plus a systemd-coredump core_pattern.
#     A slim image has neither coredumpctl nor jq, so the probe returns early
#     and generic/394, 564, 565 and 749 stop being reported as failures for
#     dumping the cores they are written to expect (they set ulimit -c 0
#     precisely to suppress them). Note core_pattern itself is NOT namespaced
#     -- a container sees the host's -- so this works by the binaries being
#     absent, not by the setting differing.
#
# What it does NOT change, measured: the NFS client and server are the host
# kernel's in both cases, so the ctime, timestamp-range and splice results are
# identical, and so is generic/103.
#
# --privileged is required (loop devices, mount, nfsd) and --net=host is
# deliberate: knfsd is only partially network-namespace aware, and sharing the
# host netns keeps loopback NFS working exactly as it does without a
# container rather than betting on rpc.nfsd in a container netns.
#
# Usage: bash scripts/00-run-xfstests-in-container.sh
#        bash scripts/00-run-xfstests-in-container.sh generic/001 generic/002
#        bash scripts/00-run-xfstests-in-container.sh -g quick
#
# Exits with xfstests' own status.

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Ubuntu to match the runner, so the only deliberate difference from the
# no-container path is that the versions are pinned. Pin to a digest for a
# result that stays reproducible after the tag moves.
CONTAINER_IMAGE="${CONTAINER_IMAGE:-ubuntu:24.04}"
CONTAINER_NAME="${CONTAINER_NAME:-nfs-xfstests-run}"

# The backing images must live on a real host filesystem, not in the
# container's overlay writable layer: they reach tens of GB and overlay is a
# poor place to grow a 30G sparse file. Bind-mounted at the same path inside
# and out so the inner script's paths and the log agree.
NFS_LOOPBACK_BASE="${NFS_LOOPBACK_BASE:-/srv/nfs-test-env}"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "docker not found"
[ -f "${REPO_ROOT}/xfstests/check" ] ||
    die "no xfstests at ${REPO_ROOT}/xfstests -- git submodule update --init xfstests"
[ -f "${REPO_ROOT}/scripts/00-run-xfstests-on-gh-ci.sh" ] ||
    die "the inner script is missing from ${REPO_ROOT}/scripts"

# The kernel side is the host's, so the host must be able to host knfsd
# whatever the container does. Failing here names the cause; inside the
# container the same problem surfaces as an opaque rpc.nfsd error.
sudo_maybe=""
[ "$(id -u)" -eq 0 ] || sudo_maybe="sudo"
$sudo_maybe modprobe nfsd ||
    die "cannot load nfsd on the host -- a container cannot supply it"

teardown() {
    local rc=$?
    log "removing the container"
    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    return $rc
}
trap teardown EXIT

docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
$sudo_maybe mkdir -p "$NFS_LOOPBACK_BASE"

log "running xfstests in $CONTAINER_IMAGE"
echo "  repo:        ${REPO_ROOT} -> /repo"
echo "  backing:     ${NFS_LOOPBACK_BASE} (bind-mounted from the host)"
echo "  check args:  ${*:-(the inner default)}"

# --pid=host is NOT set: the inner script's pidof calls should see the
# container's own daemons, not the host's nfs-server if one happens to run.
#
# /lib/modules is read-only so modprobe inside can find modules if it tries;
# the module is already loaded above, so this is belt and braces.
docker run --rm --name "$CONTAINER_NAME" \
    --privileged \
    --net=host \
    -v "${REPO_ROOT}:/repo" \
    -v "${NFS_LOOPBACK_BASE}:${NFS_LOOPBACK_BASE}" \
    -v /lib/modules:/lib/modules:ro \
    -e NFS_LOOPBACK_BASE="$NFS_LOOPBACK_BASE" \
    -e REPO_ROOT=/repo \
    -e XFSTESTS_DIR=/repo/xfstests \
    -e "NFS_LOOPBACK_IMG_SIZE=${NFS_LOOPBACK_IMG_SIZE:-}" \
    -e "NFS_LOOPBACK_IMG_MAX=${NFS_LOOPBACK_IMG_MAX:-}" \
    -e "RECLAIM_DISK=${RECLAIM_DISK:-0}" \
    -w /repo \
    "$CONTAINER_IMAGE" \
    bash /repo/scripts/00-run-xfstests-on-gh-ci.sh "$@"
