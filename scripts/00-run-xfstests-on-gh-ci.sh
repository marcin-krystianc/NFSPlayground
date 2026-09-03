#!/bin/bash
# One self-contained script: run xfstests against NFS on a GitHub Actions
# runner, using the runner's own kernel modules. The sibling of
# 00-run-xfstests-on-vm-and-docker.sh, for the case where there is one
# disposable machine and no docker.
#
# The difference from the docker script is where the server comes from. There
# the servers are containers on a bridge network; here the runner exports two
# xfs filesystems from its own knfsd over loopback and mounts them back with
# its own NFS client. So this tests the client and server pair the runner's
# kernel ships -- which also means it says nothing about any other kernel,
# VAST's included.
#
# The exports are loop-mounted xfs images rather than directories on the
# runner's root filesystem, because the backing filesystem is a test
# parameter that xfstests cannot see: FSTYP is "nfs" and nothing probes the
# server. See NFS_LOOPBACK_IMG_SIZE below for what that costs when it is
# ext4. scripts/nfs-test-env/check-xfs-backing-store.sh asserts the
# capacities the suite assumes.
#
# Standalone on purpose: it does not source env.sh and calls no sibling
# script, so a CI job is one line and a throwaway VM can run the identical
# thing. The cost is duplication with 01-setup-loopback-server.sh and
# 02-configure-xfstests.sh; change one, check the other.
#
# It installs packages and creates users, which is fine on a disposable
# runner and rude anywhere else -- hence INSTALL_DEPS, on by default only
# because CI is the intended caller. Set INSTALL_DEPS=0 to skip.
#
# Usage: bash scripts/00-run-xfstests-on-gh-ci.sh
#        bash scripts/00-run-xfstests-on-gh-ci.sh generic/001 generic/002
#        bash scripts/00-run-xfstests-on-gh-ci.sh -g quick        # 645 tests, hours
#
# Exits with xfstests' own status.

set -euo pipefail

# ---------------------------------------------------------------------------
# Config. Defaults match scripts/nfs-test-env/env.sh where they overlap.
# ---------------------------------------------------------------------------
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
XFSTESTS_DIR="${XFSTESTS_DIR:-${REPO_ROOT}/xfstests}"

NFS_LOOPBACK_BASE="${NFS_LOOPBACK_BASE:-/srv/nfs-test-env}"
NFS_LOOPBACK_CLIENT="${NFS_LOOPBACK_CLIENT:-127.0.0.1}"
NFS_LOOPBACK_VERS="${NFS_LOOPBACK_VERS:-4.2}"

# Distinct so xfstests still sees two filesystems: it mkfs's and remounts
# SCRATCH_DEV freely, so SCRATCH cannot be the filesystem TEST_DIR lives on.
NFS_TEST_FSID="${NFS_TEST_FSID:-1}"
NFS_SCRATCH_FSID="${NFS_SCRATCH_FSID:-2}"

# Each export is its own xfs filesystem in a loop-mounted sparse image rather
# than a directory on the runner's root filesystem. Two reasons, both of them
# things a plain directory cannot give:
#
#  1. xfstests keys its per-filesystem limits on FSTYP, which over NFS is
#     "nfs" and says nothing about the server's backing store -- there is no
#     probe for it. tests/generic/020's _attr_get_max groups nfs with xfs and
#     asserts max_attrs=1000; generic/486 asserts a 65536-byte xattr value.
#     ext4 with 4k blocks gives 112 attrs and caps a value at one block, so
#     both fail with ENOSPC against an ext4-backed export. xfs satisfies both.
#  2. generic/103 is an ENOSPC test: _consume_freesp fallocates all free space
#     bar ~512kB. Against a directory on the root filesystem that fills the
#     runner's own disk, which is how a previous run got "tac: /tmp/...: write
#     error: No space left on device" out of unrelated tests. A sized image
#     bounds the damage, and separate images mean filling SCRATCH cannot
#     starve TEST.
#
# Sparse, so the size is a ceiling and not an upfront cost -- only generic/103
# and friends ever inflate it. Must be >=300MB or so, which is mkfs.xfs's own
# minimum.
NFS_LOOPBACK_IMG_SIZE="${NFS_LOOPBACK_IMG_SIZE:-4G}"
NFS_LOOPBACK_IMG_DIR="${NFS_LOOPBACK_IMG_DIR:-${NFS_LOOPBACK_BASE}/images}"

TEST_MNT="${TEST_MNT:-/mnt/nfs-test-env/test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/nfs-test-env/scratch}"

INSTALL_DEPS="${INSTALL_DEPS:-1}"

# Optional: one test per line, xfstests -E format. Empty today -- a test earns
# a place only with a comment saying why, so the file stays a record of
# decisions rather than a way to get CI green.
EXCLUDE_FILE="${EXCLUDE_FILE:-${REPO_ROOT}/scripts/nfs-test-env/xfstests-exclude}"

# Not "-g quick": that is 645 tests for NFS and runs for hours, because the
# cost is per-test mount and sync latency, not CPU (generic/007 alone is ~6
# minutes). attr+acl+dir is 61 tests covering xattrs, ACLs and directory
# operations, ~14 minutes measured. Note it is a different slice of the suite
# rather than a subset -- it includes tests quick does not select.
DEFAULT_CHECK_ARGS=(-g attr -g acl -g dir)

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

check_args=("$@")
[ ${#check_args[@]} -gt 0 ] || check_args=("${DEFAULT_CHECK_ARGS[@]}")

[ -f "${XFSTESTS_DIR}/check" ] ||
    die "no xfstests at ${XFSTESTS_DIR} -- git submodule update --init xfstests"

# ---------------------------------------------------------------------------
# Dependencies and users
# ---------------------------------------------------------------------------
if [ "$INSTALL_DEPS" = "1" ]; then
    log "installing packages"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        --no-install-recommends \
        nfs-kernel-server nfs-common \
        build-essential autoconf automake libtool-bin pkg-config gettext \
        uuid-dev libattr1-dev libacl1-dev libaio-dev libgdbm-dev \
        xfslibs-dev xfsprogs e2fsprogs attr acl quota ||
        die "package install failed"
fi

command -v exportfs >/dev/null ||
    die "exportfs not found -- install nfs-kernel-server (or set INSTALL_DEPS=1)"
command -v mkfs.xfs >/dev/null ||
    die "mkfs.xfs not found -- install xfsprogs (or set INSTALL_DEPS=1)"

# Many generic tests call _require_user and are silently skipped without
# these. The numeric-leading third name is xfstests' own: it checks that such
# a username is handled (common/rc's _require_user 123456-fsgqa).
log "creating the fsgqa test users"
sudo groupadd -f fsgqa
for u in fsgqa fsgqa2 123456-fsgqa; do
    id "$u" >/dev/null 2>&1 ||
        sudo useradd -m -g fsgqa "$u" 2>/dev/null ||
        sudo useradd -M -g fsgqa "$u" ||
        warn "could not create user $u -- tests needing it will be skipped"
done

# ---------------------------------------------------------------------------
# Teardown, registered before anything is exported. Polite rather than
# necessary on a disposable runner, but it makes the script re-runnable on a
# real machine. Must not change the status we are exiting with.
# ---------------------------------------------------------------------------
teardown() {
    local rc=$?

    log "tearing down"
    for m in "$TEST_MNT" "$SCRATCH_MNT"; do
        if mountpoint -q "$m" 2>/dev/null; then
            sudo umount "$m" 2>/dev/null || sudo umount -l "$m" 2>/dev/null ||
                warn "could not unmount $m"
        fi
    done
    sudo sed -i '/# BEGIN nfs-test-env ci/,/# END nfs-test-env ci/d' \
        /etc/exports 2>/dev/null || true
    sudo exportfs -ra 2>/dev/null || true

    # After unexporting, or nfsd still holds a reference and the umount is
    # EBUSY. umount detaches the loop device it set up.
    for name in test scratch; do
        mnt="${NFS_LOOPBACK_BASE}/${name}"
        if mountpoint -q "$mnt" 2>/dev/null; then
            sudo umount "$mnt" 2>/dev/null || sudo umount -l "$mnt" 2>/dev/null ||
                warn "could not unmount the backing filesystem $mnt"
        fi
        sudo rm -f "${NFS_LOOPBACK_IMG_DIR}/${name}.img" 2>/dev/null || true
    done
    return $rc
}
trap teardown EXIT

# ---------------------------------------------------------------------------
# The NFS server: the runner's own knfsd
# ---------------------------------------------------------------------------
log "loading nfsd"
sudo modprobe nfsd || die "cannot load nfsd -- this runner cannot host knfsd"

log "creating the xfs backing filesystems under $NFS_LOOPBACK_BASE"
sudo mkdir -p "$NFS_LOOPBACK_IMG_DIR"
for name in test scratch; do
    img="${NFS_LOOPBACK_IMG_DIR}/${name}.img"
    mnt="${NFS_LOOPBACK_BASE}/${name}"

    sudo mkdir -p "$mnt"
    # A leftover mount from an interrupted run would otherwise be silently
    # exported instead of the fresh filesystem.
    if mountpoint -q "$mnt" 2>/dev/null; then
        sudo umount "$mnt" 2>/dev/null || sudo umount -l "$mnt" 2>/dev/null ||
            die "$mnt is mounted and will not unmount"
    fi

    sudo rm -f "$img"
    sudo truncate -s "$NFS_LOOPBACK_IMG_SIZE" "$img" ||
        die "could not create $img -- is there ${NFS_LOOPBACK_IMG_SIZE} free?"
    sudo mkfs.xfs -q "$img" || die "mkfs.xfs failed on $img"
    # mount -o loop attaches the loop device itself and umount detaches it, so
    # teardown needs no losetup -d.
    sudo mount -o loop "$img" "$mnt" ||
        die "could not loop-mount $img at $mnt -- is the loop module available?"
    # xfstests runs as root and chowns freely. After the mount, not before:
    # the mount would hide a chmod applied to the underlying directory.
    sudo chmod 777 "$mnt"
    echo "  $mnt <- $img ($(findmnt -no FSTYPE "$mnt"), ${NFS_LOOPBACK_IMG_SIZE} max)"
done

# Replace only our own block, so an unrelated NFS setup on the machine
# survives and re-running is safe.
log "writing /etc/exports"
sudo sed -i '/# BEGIN nfs-test-env ci/,/# END nfs-test-env ci/d' \
    /etc/exports 2>/dev/null || true
sudo tee -a /etc/exports >/dev/null <<EOF
# BEGIN nfs-test-env ci -- managed by 00-run-xfstests-on-gh-ci.sh
${NFS_LOOPBACK_BASE}/test ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_TEST_FSID})
${NFS_LOOPBACK_BASE}/scratch ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_SCRATCH_FSID})
# END nfs-test-env ci
EOF

# Start the server before exportfs -ra: on a fresh machine rpc.mountd is not
# running yet, and exportfs alone leaves nothing serving.
log "starting the NFS server"
sudo systemctl enable --now nfs-server 2>/dev/null ||
    sudo systemctl restart nfs-server ||
    die "could not start nfs-server -- 'journalctl -u nfs-server'"

log "applying exports"
sudo exportfs -ra
sudo exportfs -v

# Prove the export serves before handing over. xfstests reports an unusable
# TEST_DEV as a confusing _notrun much later, or as a mount failure with no
# hint of which side is at fault.
log "verifying the export mounts and is writable"
probe="$(mktemp -d)"
if ! sudo mount -t nfs -o "vers=${NFS_LOOPBACK_VERS}" \
        "127.0.0.1:${NFS_LOOPBACK_BASE}/test" "$probe"; then
    rmdir "$probe"
    die "cannot mount the test export -- see 'exportfs -v' and 'journalctl -u nfs-server'"
fi
if ! sudo touch "${probe}/.probe" 2>/dev/null; then
    sudo umount "$probe"; rmdir "$probe"
    die "the export mounted but is not writable"
fi
sudo rm -f "${probe}/.probe"
sudo umount "$probe"
rmdir "$probe"

# ---------------------------------------------------------------------------
# xfstests
# ---------------------------------------------------------------------------
if [ -x "${XFSTESTS_DIR}/ltp/fsstress" ]; then
    log "xfstests already built"
else
    log "building xfstests"
    make -C "$XFSTESTS_DIR" -j"$(nproc)" >/tmp/xfstests-build.log 2>&1 ||
        die "xfstests build failed -- see /tmp/xfstests-build.log"
fi

sudo mkdir -p "$TEST_MNT" "$SCRATCH_MNT"

config="${XFSTESTS_DIR}/local.config"
log "writing $config"
cat > "$config" <<EOF
# generated by 00-run-xfstests-on-gh-ci.sh -- do not edit, it is overwritten
export FSTYP=nfs
export TEST_DEV=127.0.0.1:${NFS_LOOPBACK_BASE}/test
export TEST_DIR=${TEST_MNT}
export SCRATCH_DEV=127.0.0.1:${NFS_LOOPBACK_BASE}/scratch
export SCRATCH_MNT=${SCRATCH_MNT}
EOF
cat "$config"

# What is actually under test, recorded so a result is attributable to a
# specific kernel and nfs-utils rather than to "NFS".
log "under test"
echo "  kernel:         $(uname -r)"
echo "  nfs-utils:      $(dpkg-query -W -f='${Version}' nfs-common 2>/dev/null || echo unknown)"
echo "  server-side fs: $(findmnt -no FSTYPE -T "${NFS_LOOPBACK_BASE}/test" 2>/dev/null || echo unknown)"

exclude=()
if [ -s "$EXCLUDE_FILE" ] && grep -qvE '^\s*(#|$)' "$EXCLUDE_FILE"; then
    log "excluding tests listed in $EXCLUDE_FILE"
    exclude=(-E "$EXCLUDE_FILE")
fi

log "running xfstests: ${check_args[*]}"
status=0
set +e
( cd "$XFSTESTS_DIR" && sudo ./check -nfs "${exclude[@]}" "${check_args[@]}" )
status=$?
set -e

if [ "$status" -eq 0 ]; then
    log "xfstests passed"
else
    log "xfstests failed (status $status) -- see ${XFSTESTS_DIR}/results/"
fi
exit "$status"
