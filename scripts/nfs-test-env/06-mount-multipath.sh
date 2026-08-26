#!/bin/bash
# Mount with VAST's remoteports=/nconnect= multipath options across the
# NFS_SERVER_COUNT docker servers, and show the per-transport spread.
#
# No xfstests case exercises this (checked: no hits for nconnect, pconnect,
# remoteports, localports anywhere in xfstests/tests or xfstests/common).
# This script is the manual substitute. Mount option syntax and the
# rpc-clients output shape are from
# vastnfs-4.5.8/docs/src/usage/mount-params.md and
# vastnfs-4.5.8/docs/src/usage/vastnfs-ctl.md ("Show RPC Clients").
#
# Requires the VAST modules to actually be loaded (05-verify-vastnfs.sh) --
# remoteports= is a VAST-only mount option and will be rejected by the
# inbox client.
#
# Usage: scripts/nfs-test-env/06-mount-multipath.sh [nconnect]

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

command -v vastnfs-ctl >/dev/null || die "vastnfs-ctl not found -- install VAST modules first (04, 05)"

nconnect="${1:-${NFS_SERVER_COUNT}}"

first="$(server_ip 1)"
last="$(server_ip "$NFS_SERVER_COUNT")"
mnt="${TEST_MNT}"

sudo mkdir -p "$mnt"
if mountpoint -q "$mnt"; then
    log "unmounting stale $mnt"
    sudo umount "$mnt"
fi

log "mounting $first:/export at $mnt with nconnect=${nconnect},remoteports=${first}-${last}"
sudo mount -t nfs -o "vers=3,nconnect=${nconnect},remoteports=${first}-${last}" \
    "${first}:/export" "$mnt"

log "touching a file to generate traffic"
sudo touch "${mnt}/nfs-test-env-touch"

log "vastnfs-ctl rpc-clients $mnt"
sudo vastnfs-ctl rpc-clients "$mnt"

echo
echo "Look for more than one distinct remote_port_idx across the xprt: lines"
echo "above -- that is the signal that traffic is spread across ${first}..${last}"
echo "rather than opening ${nconnect} connections to a single address (the"
echo "upstream-only nconnect behavior)."
