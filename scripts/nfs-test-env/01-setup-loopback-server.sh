#!/bin/bash
# Export two directories over NFS from the local machine's own knfsd, for
# xfstests to run against. A loopback alternative to 01-setup-servers.sh:
# same shape (TEST_DEV plus a separate SCRATCH_DEV), no docker, no second
# host -- whatever NFS client and server modules are currently loaded are
# what gets tested.
#
# This is what the CI workflow uses (.github/workflows/xfstests.yml), and it
# is deliberately the same script CI runs so a local reproduction is exact.
#
# Two exports rather than one because xfstests needs them: _require_scratch
# mkfs's and remounts SCRATCH_DEV freely, so it cannot be the filesystem
# TEST_DIR lives on. They get distinct fsids for the same reason -- an
# identical fsid would make the two exports one filesystem as far as the
# client is concerned.
#
# The server-side filesystem is whatever backs NFS_LOOPBACK_BASE (on a
# GitHub runner, the root ext4). That decides which tests are meaningful:
# server-side capability, not client-side, is what gates xattrs, ACLs and
# fallocate over NFS.
#
# Usage: scripts/nfs-test-env/01-setup-loopback-server.sh
#        NFS_LOOPBACK_BASE=/mnt/big scripts/nfs-test-env/01-setup-loopback-server.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

command -v exportfs >/dev/null ||
    die "exportfs not found -- install nfs-kernel-server"

log "loading nfsd"
sudo modprobe nfsd || die "cannot load nfsd"

log "creating exports under $NFS_LOOPBACK_BASE"
sudo mkdir -p "${NFS_LOOPBACK_BASE}/test" "${NFS_LOOPBACK_BASE}/scratch"
# xfstests runs as root and chowns freely; both need to be writable by it.
sudo chmod 777 "${NFS_LOOPBACK_BASE}/test" "${NFS_LOOPBACK_BASE}/scratch"

# Replace our own block in /etc/exports and leave anything else alone, so
# re-running is safe and an existing NFS setup on the machine survives.
log "writing /etc/exports"
sudo sed -i '/# BEGIN nfs-test-env loopback/,/# END nfs-test-env loopback/d' \
    /etc/exports 2>/dev/null || true
sudo tee -a /etc/exports >/dev/null <<EOF
# BEGIN nfs-test-env loopback -- managed by 01-setup-loopback-server.sh
${NFS_LOOPBACK_BASE}/test ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_TEST_FSID})
${NFS_LOOPBACK_BASE}/scratch ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_SCRATCH_FSID})
# END nfs-test-env loopback
EOF

# Start the server before exportfs -ra: on a fresh machine rpc.mountd is not
# running yet and exportfs alone leaves nothing serving.
log "starting the NFS server"
sudo systemctl enable --now nfs-server 2>/dev/null ||
    sudo systemctl restart nfs-server ||
    die "could not start nfs-server"

log "applying exports"
sudo exportfs -ra
sudo exportfs -v

log "creating mount points"
sudo mkdir -p "$TEST_MNT" "$SCRATCH_MNT"

# Prove the export actually serves before handing over to xfstests, which
# reports an unusable TEST_DEV as a confusing _notrun much later.
log "verifying the test export mounts and is writable"
probe="$(mktemp -d)"
sudo mount -t nfs -o "vers=${NFS_LOOPBACK_VERS}" \
    "127.0.0.1:${NFS_LOOPBACK_BASE}/test" "$probe" ||
    die "cannot mount the test export -- see 'exportfs -v' and journalctl -u nfs-server"
if ! sudo touch "${probe}/.probe" 2>/dev/null; then
    sudo umount "$probe"; rmdir "$probe"
    die "the test export mounted but is not writable"
fi
sudo rm -f "${probe}/.probe"
sudo umount "$probe"
rmdir "$probe"

log "loopback NFS server ready (NFSv${NFS_LOOPBACK_VERS})"
