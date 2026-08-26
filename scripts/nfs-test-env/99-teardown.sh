#!/bin/bash
# Tear down the docker NFS servers and network from 01-setup-servers.sh, and
# unmount TEST_MNT/SCRATCH_MNT if still mounted. Does not uninstall the VAST
# kernel modules -- that's a host-wide change outside this script's scope,
# see vastnfs-4.5.8/docs/src/UNINSTALL.md.
#
# Usage: scripts/nfs-test-env/99-teardown.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

for m in "$TEST_MNT" "$SCRATCH_MNT"; do
    if mountpoint -q "$m" 2>/dev/null; then
        log "unmounting $m"
        sudo umount "$m"
    fi
done

for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    name="$(server_name "$i")"
    if docker inspect "$name" >/dev/null 2>&1; then
        log "removing $name"
        docker rm -f "$name" >/dev/null
    fi
done

if docker network inspect "$NFS_NET" >/dev/null 2>&1; then
    log "removing docker network $NFS_NET"
    docker network rm "$NFS_NET" >/dev/null
fi
