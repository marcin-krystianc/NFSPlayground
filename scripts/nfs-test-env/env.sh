#!/bin/bash
# Shared config for the nfs-test-env scripts. Source, don't execute.
#
# Override any of these before calling the scripts, e.g.:
#   NFS_SERVER_COUNT=4 ./scripts/nfs-test-env/01-setup-servers.sh

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

NFS_NET="${NFS_NET:-nfstestnet}"
NFS_SUBNET="${NFS_SUBNET:-172.28.0.0/24}"
NFS_IP_PREFIX="${NFS_IP_PREFIX:-172.28.0.1}"   # server $i gets ${NFS_IP_PREFIX}$i
NFS_SERVER_COUNT="${NFS_SERVER_COUNT:-3}"      # >=2: index 1 is TEST_DEV, 2 is SCRATCH_DEV
NFS_SERVER_IMAGE="${NFS_SERVER_IMAGE:-erichough/nfs-server}"
NFS_EXPORT_BASE="${NFS_EXPORT_BASE:-${HOME}/nfs-test-env-exports}"

# Every server exports both directories with these fsids. The fsid must be
# identical across servers for a filehandle from one node to be valid on
# another (see 01-setup-servers.sh), and distinct between test and scratch
# so xfstests still sees two separate filesystems.
NFS_TEST_FSID="${NFS_TEST_FSID:-1}"
NFS_SCRATCH_FSID="${NFS_SCRATCH_FSID:-2}"

TEST_MNT="${TEST_MNT:-/mnt/nfs-test-env/test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/nfs-test-env/scratch}"

VASTNFS_DIR="${VASTNFS_DIR:-${REPO_ROOT}/vastnfs-4.5.8}"
XFSTESTS_DIR="${XFSTESTS_DIR:-${REPO_ROOT}/xfstests}"

server_ip() { echo "${NFS_IP_PREFIX}$1"; }
server_name() { echo "nfs-test-env-$1"; }

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
