#!/bin/bash
# Bring up NFS_SERVER_COUNT vanilla NFS servers in Docker, each with its own
# static IP on a dedicated bridge network. Idempotent: re-running skips
# containers/network that already exist.
#
# Not verified against this repo -- erichough/nfs-server is a general-purpose
# choice, not something pinned or tested elsewhere in this tree. Swap
# NFS_SERVER_IMAGE if it doesn't suit your host.
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

for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    name="$(server_name "$i")"
    ip="$(server_ip "$i")"
    export_dir="${NFS_EXPORT_BASE}/${i}"

    if docker inspect "$name" >/dev/null 2>&1; then
        log "$name already exists, skipping"
        continue
    fi

    sudo mkdir -p "$export_dir"
    sudo chmod 777 "$export_dir"

    log "starting $name at $ip, exporting $export_dir"
    docker run -d --name "$name" --privileged --network "$NFS_NET" --ip "$ip" \
        -v "${export_dir}:/export" \
        -e SHARED_DIRECTORY=/export \
        "$NFS_SERVER_IMAGE" >/dev/null
done

log "servers up:"
for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    echo "  $(server_ip "$i") -> ${NFS_EXPORT_BASE}/${i}  (container $(server_name "$i"))"
done
