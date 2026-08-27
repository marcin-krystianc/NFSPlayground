#!/bin/bash
# Bring up NFS_SERVER_COUNT NFS servers in Docker, each with its own static
# IP on a dedicated bridge network. Idempotent: re-running skips
# containers/network that already exist.
#
# The servers form a crude *cluster*: every container exports the same two
# host directories (test and scratch), each with an fsid that is identical
# across all containers. Linux nfsd filehandles encode fsid + inode +
# generation, so identical fsid over identical backing inodes means a
# filehandle issued by one node is valid on every other node -- which is
# what lets remoteports= spread RPCs for one mount across all of them.
#
# Verified on the test VM with xfstests' own src/open_by_handle: handles
# saved from node A (-o) open successfully against node B (-i) under this
# layout, and fail with ESTALE (errno 116) when the nodes have independent
# backing directories, which is how this script used to be written.
#
# What this does NOT simulate: NLM lock state and the duplicate reply cache
# are per-nfsd, and each nfsd keeps its own attribute cache over the shared
# backing store, so this is not coherent under genuinely concurrent
# multi-node access the way a real VAST cluster is.
#
# NFS_EXPORT_BASE must already be writable by the caller (defaults under
# $HOME so no sudo is needed).
#
# Usage: scripts/nfs-test-env/01-setup-servers.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

command -v docker >/dev/null || die "docker not found"

if ! docker network inspect "$NFS_NET" >/dev/null 2>&1; then
    log "creating docker network $NFS_NET ($NFS_SUBNET)"
    docker network create --subnet "$NFS_SUBNET" "$NFS_NET" >/dev/null
else
    log "docker network $NFS_NET already exists"
fi

# Shared backing store, exported by every node. The root is bind-mounted
# (not just its children) so that the NFSv4 pseudo-root filehandle is also
# identical across nodes -- a container-local root directory would have a
# different inode per container and break v4 multipath at the root.
mkdir -p "${NFS_EXPORT_BASE}/root/test" "${NFS_EXPORT_BASE}/root/scratch"
chmod 777 "${NFS_EXPORT_BASE}/root" \
          "${NFS_EXPORT_BASE}/root/test" "${NFS_EXPORT_BASE}/root/scratch"

for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    name="$(server_name "$i")"
    ip="$(server_ip "$i")"

    if docker inspect "$name" >/dev/null 2>&1; then
        log "$name already exists, skipping"
        continue
    fi

    log "starting $name at $ip"
    docker run -d --name "$name" --privileged --network "$NFS_NET" --ip "$ip" \
        -v "${NFS_EXPORT_BASE}/root:/export" \
        -e NFS_EXPORT_0="/export *(rw,fsid=0,no_subtree_check,insecure,no_root_squash,crossmnt)" \
        -e NFS_EXPORT_1="/export/test *(rw,fsid=${NFS_TEST_FSID},no_subtree_check,insecure,no_root_squash)" \
        -e NFS_EXPORT_2="/export/scratch *(rw,fsid=${NFS_SCRATCH_FSID},no_subtree_check,insecure,no_root_squash)" \
        "$NFS_SERVER_IMAGE" >/dev/null
done

log "servers up (all export the same directories):"
for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    echo "  $(server_ip "$i")  (container $(server_name "$i"))"
done
echo "  /export         -> ${NFS_EXPORT_BASE}/root           (fsid=0, NFSv4 pseudo-root)"
echo "  /export/test    -> ${NFS_EXPORT_BASE}/root/test      (fsid=${NFS_TEST_FSID})"
echo "  /export/scratch -> ${NFS_EXPORT_BASE}/root/scratch   (fsid=${NFS_SCRATCH_FSID})"
