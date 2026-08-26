#!/bin/bash
# Confirm the VAST NFS kernel modules are actually the ones loaded (not just
# installed). Grounded in vastnfs-4.5.8/docs/src/usage/vastnfs-ctl.md
# ("Status") and docs/src/INSTALL.md ("Verification").
#
# Usage: scripts/nfs-test-env/05-verify-vastnfs.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

command -v vastnfs-ctl >/dev/null || die "vastnfs-ctl not on PATH -- did install succeed and did you reboot?"

log "vastnfs-ctl status"
sudo vastnfs-ctl status

log "cross-checking loaded sunrpc module against the built one"
loaded="$(cat /sys/module/sunrpc/srcversion 2>/dev/null || echo unknown)"
built="$(modinfo sunrpc 2>/dev/null | awk '/^srcversion:/ {print $2}')"
echo "loaded srcversion: $loaded"
echo "modinfo srcversion: $built"
[ "$loaded" = "$built" ] || warn "srcversion mismatch -- reboot, or run 'sudo vastnfs-ctl reload'"
