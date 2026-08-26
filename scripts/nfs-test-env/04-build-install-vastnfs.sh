#!/bin/bash
# Build and install the VAST NFS kernel modules for the running kernel, per
# vastnfs-4.5.8/docs/src/build/package.md and docs/src/INSTALL.md.
#
# This replaces the host's NFS client/server kernel modules system-wide (see
# docs/vastnfs-vs-linux.md, "Replacement, not coexistence"). It is NOT
# container-scoped -- it changes what every mount on this host uses,
# including the ones from 03-run-xfstests.sh.
#
# A reboot (or `vastnfs-ctl reload`, less thoroughly) is required after
# install for the new modules to actually be the ones loaded -- this script
# stops short of rebooting for you.
#
# Usage: scripts/nfs-test-env/04-build-install-vastnfs.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

[ -d "$VASTNFS_DIR" ] || die "$VASTNFS_DIR not found -- run scripts/fetch-sources.sh vastnfs"

log "kernel: $(uname -r)"
warn "check ${VASTNFS_DIR}/docs/src/build/kernels.md yourself -- this script does not verify your kernel is in the supported range, build.sh will refuse if it isn't"

cd "$VASTNFS_DIR"
log "building (./build.sh bin)"
./build.sh bin

pkg_dir="dist"
[ -d "$pkg_dir" ] || die "build did not produce a dist/ directory"

if command -v dpkg >/dev/null 2>&1; then
    deb="$(ls -1 "${pkg_dir}"/vastnfs-modules_*.deb 2>/dev/null | head -1)"
    [ -n "$deb" ] || deb="$(ls -1 "${pkg_dir}"/vastnfs-dkms_*.deb 2>/dev/null | head -1)"
    [ -n "$deb" ] || die "no vastnfs .deb found in ${pkg_dir}/, see build output above"
    log "installing $deb"
    sudo dpkg -i "$deb"
    sudo update-initramfs -u -k "$(uname -r)"
elif command -v rpm >/dev/null 2>&1; then
    rpmf="$(ls -1 "${pkg_dir}"/vastnfs-*.x86_64.rpm 2>/dev/null | grep -v debug | head -1)"
    [ -n "$rpmf" ] || die "no vastnfs .rpm found in ${pkg_dir}/, see build output above"
    log "installing $rpmf"
    sudo yum install -y "$rpmf"
    sudo dracut -f
else
    die "neither dpkg nor rpm found, install manually per ${VASTNFS_DIR}/docs/src/INSTALL.md"
fi

log "installed. REBOOT NOW, then run 05-verify-vastnfs.sh"
