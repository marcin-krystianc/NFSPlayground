#!/bin/bash
# Run xfstests against the configured NFS servers (see 02-configure-xfstests.sh).
# Whatever kernel modules are currently loaded (inbox or VAST) are what gets
# tested -- this script does not care which.
#
# Usage: scripts/nfs-test-env/03-run-xfstests.sh [check args...]
#        scripts/nfs-test-env/03-run-xfstests.sh generic/001 generic/002
#        scripts/nfs-test-env/03-run-xfstests.sh -g quick

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

[ -f "${XFSTESTS_DIR}/local.config" ] ||
    die "no local.config, run 02-configure-xfstests.sh first"
[ -x "${XFSTESTS_DIR}/check" ] ||
    die "xfstests/check not found or not built -- see xfstests/README"

cd "$XFSTESTS_DIR"
sudo ./check -nfs "$@"
