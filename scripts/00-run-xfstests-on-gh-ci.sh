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
# ext4. scripts/check-xfs-backing-store.sh asserts the capacities the suite
# assumes, and needs no NFS to run.
#
# Standalone on purpose: it sources nothing and calls no sibling script, so a
# CI job is one line and a throwaway VM can run the identical thing.
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
# Config. Every value is overridable from the environment.
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
#
# Empty means "as large as the disk allows", computed below once the space is
# actually known -- a hardcoded default is a guess that costs a whole CI run
# to disprove. Do not size this from GitHub's published figures: the docs say
# ubuntu-latest has 14GB of storage, and a measured runner reported / as
# 145G ext4 with 84G available and no separate scratch disk.
NFS_LOOPBACK_IMG_SIZE="${NFS_LOOPBACK_IMG_SIZE:-}"

# The cap is set by the reflink CoW tests, which are the hungriest thing in
# the suite. generic/154 writes blks=2000 blocks and rewrites nr=4 reflinked
# copies of it; over NFS _get_block_size returns the 1MB wsize rather than
# the server's 4k, so that is 2GB per file and ~8GB of distinct data once
# every copy has been CoW'd. Well past that on purpose: the point of a cap at
# all is only to keep generic/103 from taking the host down with it, so it
# sits high enough to be effectively unconstrained (30G each of a measured
# 84G leaves the runner ~24G, which the agent and the results artifact need).
NFS_LOOPBACK_IMG_MAX="${NFS_LOOPBACK_IMG_MAX:-30G}"
NFS_LOOPBACK_IMG_MIN="${NFS_LOOPBACK_IMG_MIN:-1G}"   # below this, give up
NFS_LOOPBACK_DISK_MARGIN="${NFS_LOOPBACK_DISK_MARGIN:-2G}"  # for apt, the build, results/
NFS_LOOPBACK_IMG_DIR="${NFS_LOOPBACK_IMG_DIR:-${NFS_LOOPBACK_BASE}/images}"

TEST_MNT="${TEST_MNT:-/mnt/nfs-test-env/test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/nfs-test-env/scratch}"

INSTALL_DEPS="${INSTALL_DEPS:-1}"

# Delete the preinstalled toolchains a hosted runner ships with, to make room
# for the backing images. GitHub documents 14GB of storage for ubuntu-latest
# and most of it is spent on toolchains this suite never touches.
#
# Off by default and deliberately not grouped with INSTALL_DEPS: installing
# packages on someone's machine is rude, but rm -rf /usr/share/dotnet is
# destructive, so it stays opt-in even though CI is the intended caller.
RECLAIM_DISK="${RECLAIM_DISK:-0}"

# Optional: one test per line, xfstests -E format. Empty today -- a test earns
# a place only with a comment saying why, so the file stays a record of
# decisions rather than a way to get CI green.
EXCLUDE_FILE="${EXCLUDE_FILE:-${REPO_ROOT}/scripts/xfstests-exclude}"

# Not "-g quick": that is 645 tests for NFS and runs for hours, because the
# cost is per-test mount and sync latency, not CPU (generic/007 alone is ~6
# minutes). attr+acl+dir is 61 tests covering xattrs, ACLs and directory
# operations, ~14 minutes measured. Note it is a different slice of the suite
# rather than a subset -- it includes tests quick does not select.
DEFAULT_CHECK_ARGS=(-g attr -g acl -g dir)

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# Everything privileged goes through $SUDO rather than a literal sudo, so this
# runs both as a normal user on a runner and as root in a container, where
# sudo is usually not installed at all. Cannot be a function named sudo: the
# env-var forms below ("$SUDO" DEBIAN_FRONTEND=... apt-get) are not valid
# calls to a shell function.
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null; then
    SUDO="sudo"
else
    die "not root and no sudo -- run as root or install sudo"
fi

check_args=("$@")
[ ${#check_args[@]} -gt 0 ] || check_args=("${DEFAULT_CHECK_ARGS[@]}")

[ -f "${XFSTESTS_DIR}/check" ] ||
    die "no xfstests at ${XFSTESTS_DIR} -- git submodule update --init xfstests"

# ---------------------------------------------------------------------------
# Dependencies and users
# ---------------------------------------------------------------------------
if [ "$INSTALL_DEPS" = "1" ]; then
    log "installing packages"
    $SUDO apt-get update -qq
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
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
$SUDO groupadd -f fsgqa
for u in fsgqa fsgqa2 123456-fsgqa; do
    id "$u" >/dev/null 2>&1 ||
        $SUDO useradd -m -g fsgqa "$u" 2>/dev/null ||
        $SUDO useradd -M -g fsgqa "$u" ||
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
            $SUDO umount "$m" 2>/dev/null || $SUDO umount -l "$m" 2>/dev/null ||
                warn "could not unmount $m"
        fi
    done
    $SUDO sed -i '/# BEGIN nfs-test-env ci/,/# END nfs-test-env ci/d' \
        /etc/exports 2>/dev/null || true
    $SUDO exportfs -ra 2>/dev/null || true

    # After unexporting, or nfsd still holds a reference and the umount is
    # EBUSY. umount detaches the loop device it set up.
    for name in test scratch; do
        mnt="${NFS_LOOPBACK_BASE}/${name}"
        if mountpoint -q "$mnt" 2>/dev/null; then
            $SUDO umount "$mnt" 2>/dev/null || $SUDO umount -l "$mnt" 2>/dev/null ||
                warn "could not unmount the backing filesystem $mnt"
        fi
        $SUDO rm -f "${NFS_LOOPBACK_IMG_DIR}/${name}.img" 2>/dev/null || true
    done
    return $rc
}
trap teardown EXIT

# ---------------------------------------------------------------------------
# The NFS server: the runner's own knfsd
# ---------------------------------------------------------------------------
log "loading nfsd"
$SUDO modprobe nfsd || die "cannot load nfsd -- this runner cannot host knfsd"

# The images are sparse but generic/103 and friends inflate them to their
# full size, so what matters is the free space on whatever holds them. Report
# every local filesystem: on a hosted runner the root filesystem is the small
# one and there is usually a much larger scratch disk elsewhere, and knowing
# which is which is the difference between choosing NFS_LOOPBACK_IMG_DIR and
# guessing. A previous run drove the root filesystem to 0 MB and the runner
# itself warned about it.
log "disk space before setup"
df -h -x tmpfs -x devtmpfs -x overlay -x squashfs \
    --output=target,fstype,size,used,avail,pcent 2>/dev/null | sed 's/^/  /' ||
    warn "could not read df"

if [ "$RECLAIM_DISK" = "1" ]; then
    # Reported per path rather than as a total, because which of these exists
    # and what it costs is exactly what is undocumented -- the log becomes the
    # measurement for the next time this needs tuning.
    log "reclaiming disk space"
    for path in /usr/share/dotnet /usr/local/lib/android /opt/ghc \
                /usr/local/.ghcup /opt/hostedtoolcache /usr/local/share/powershell \
                /usr/local/share/chromium /usr/local/lib/node_modules; do
        [ -e "$path" ] || continue
        size="$($SUDO du -sh "$path" 2>/dev/null | cut -f1)"
        if $SUDO rm -rf "$path" 2>/dev/null; then
            echo "  freed ${size:-?} from $path"
        else
            warn "could not remove $path"
        fi
    done
    if command -v docker >/dev/null; then
        reclaimed="$($SUDO docker system prune -af 2>/dev/null | tail -1)"
        echo "  docker: ${reclaimed:-nothing to prune}"
    fi

    log "disk space after reclaiming"
    df -h -x tmpfs -x devtmpfs -x overlay -x squashfs \
        --output=target,fstype,size,used,avail,pcent 2>/dev/null | sed 's/^/  /' ||
        warn "could not read df"
fi

log "creating the xfs backing filesystems under $NFS_LOOPBACK_BASE"
$SUDO mkdir -p "$NFS_LOOPBACK_IMG_DIR"

# Both images can reach their full size at once (generic/103 fills one, the
# reflink CoW tests fill the other), so the budget is 2 x size + a margin for
# apt, the xfstests build and results/.
margin_bytes="$(numfmt --from=iec "$NFS_LOOPBACK_DISK_MARGIN")"
have="$(df -B1 --output=avail "$NFS_LOOPBACK_IMG_DIR" | tail -1)"
echo "  $NFS_LOOPBACK_IMG_DIR is on $(findmnt -no TARGET,FSTYPE -T "$NFS_LOOPBACK_IMG_DIR")"
echo "  $(numfmt --to=iec "$have") available, keeping ${NFS_LOOPBACK_DISK_MARGIN} in reserve"

if [ -n "$NFS_LOOPBACK_IMG_SIZE" ]; then
    img_bytes="$(numfmt --from=iec "$NFS_LOOPBACK_IMG_SIZE")" ||
        die "NFS_LOOPBACK_IMG_SIZE=${NFS_LOOPBACK_IMG_SIZE} is not a size numfmt understands"
    need=$((img_bytes * 2 + margin_bytes))
    [ "$have" -ge "$need" ] ||
        die "NFS_LOOPBACK_IMG_SIZE=${NFS_LOOPBACK_IMG_SIZE} needs $(numfmt --to=iec "$need") but only $(numfmt --to=iec "$have") is available.
    Lower it, raise RECLAIM_DISK, or point NFS_LOOPBACK_IMG_DIR at a bigger
    filesystem -- see the df above."
else
    # Fit to what is there rather than guessing: a wrong hardcoded default
    # either wastes a run failing this check or hits ENOSPC mid-suite.
    img_bytes=$(( (have - margin_bytes) / 2 ))
    max_bytes="$(numfmt --from=iec "$NFS_LOOPBACK_IMG_MAX")"
    [ "$img_bytes" -le "$max_bytes" ] || img_bytes="$max_bytes"
    min_bytes="$(numfmt --from=iec "$NFS_LOOPBACK_IMG_MIN")"
    [ "$img_bytes" -ge "$min_bytes" ] ||
        die "only $(numfmt --to=iec "$have") available, which leaves less than
    ${NFS_LOOPBACK_IMG_MIN} per backing image after a ${NFS_LOOPBACK_DISK_MARGIN} margin.
    Set RECLAIM_DISK=1, or point NFS_LOOPBACK_IMG_DIR at a bigger filesystem."
    echo "  sized each image at $(numfmt --to=iec "$img_bytes") (set NFS_LOOPBACK_IMG_SIZE to override)"
fi

# truncate rejects fractional suffixes ("4.5G" is an invalid number), and
# numfmt --to=iec produces exactly those, so bytes are the only safe currency
# from here on. The IEC string is for humans.
img_bytes=$(( img_bytes / 1048576 * 1048576 ))
img_human="$(numfmt --to=iec "$img_bytes")"

for name in test scratch; do
    img="${NFS_LOOPBACK_IMG_DIR}/${name}.img"
    mnt="${NFS_LOOPBACK_BASE}/${name}"

    $SUDO mkdir -p "$mnt"
    # A leftover mount from an interrupted run would otherwise be silently
    # exported instead of the fresh filesystem.
    if mountpoint -q "$mnt" 2>/dev/null; then
        $SUDO umount "$mnt" 2>/dev/null || $SUDO umount -l "$mnt" 2>/dev/null ||
            die "$mnt is mounted and will not unmount"
    fi

    $SUDO rm -f "$img"
    $SUDO truncate -s "$img_bytes" "$img" ||
        die "could not create $img -- is there ${img_human} free?"
    $SUDO mkfs.xfs -q "$img" || die "mkfs.xfs failed on $img"
    # mount -o loop attaches the loop device itself and umount detaches it, so
    # teardown needs no losetup -d.
    $SUDO mount -o loop "$img" "$mnt" ||
        die "could not loop-mount $img at $mnt -- is the loop module available?"
    # xfstests runs as root and chowns freely. After the mount, not before:
    # the mount would hide a chmod applied to the underlying directory.
    $SUDO chmod 777 "$mnt"
    echo "  $mnt <- $img ($(findmnt -no FSTYPE "$mnt"), ${img_human} max)"
done

# Replace only our own block, so an unrelated NFS setup on the machine
# survives and re-running is safe.
log "writing /etc/exports"
$SUDO sed -i '/# BEGIN nfs-test-env ci/,/# END nfs-test-env ci/d' \
    /etc/exports 2>/dev/null || true
$SUDO tee -a /etc/exports >/dev/null <<EOF
# BEGIN nfs-test-env ci -- managed by 00-run-xfstests-on-gh-ci.sh
${NFS_LOOPBACK_BASE}/test ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_TEST_FSID})
${NFS_LOOPBACK_BASE}/scratch ${NFS_LOOPBACK_CLIENT}(rw,sync,no_subtree_check,no_root_squash,fsid=${NFS_SCRATCH_FSID})
# END nfs-test-env ci
EOF

# Start the server before exportfs -ra: on a fresh machine rpc.mountd is not
# running yet, and exportfs alone leaves nothing serving.
#
# Two paths because there are two hosts. A runner has systemd and the
# nfs-server unit does the right ordering for free. A container has no PID 1
# to ask, so the daemons are started by hand -- the unit's own ExecStart
# sequence, minus the parts systemd would do. Detected by asking systemd
# whether it is actually running, not by looking for the systemctl binary:
# the binary is present in plenty of images where nothing is listening on
# /run/systemd/system.
log "starting the NFS server"
if [ -d /run/systemd/system ] && command -v systemctl >/dev/null; then
    $SUDO systemctl enable --now nfs-server 2>/dev/null ||
        $SUDO systemctl restart nfs-server ||
        die "could not start nfs-server -- 'journalctl -u nfs-server'"
else
    echo "  no systemd -- starting rpcbind, rpc.nfsd and rpc.mountd directly"
    # rpcbind is still needed: NFSv4 does not use it, but rpc.mountd
    # registers regardless and fails noisily without it.
    if ! pidof rpcbind >/dev/null 2>&1; then
        $SUDO rpcbind -w 2>/dev/null || warn "rpcbind did not start"
    fi
    # nfsd needs its own procfs pseudo-filesystem; the unit mounts this.
    if ! mountpoint -q /proc/fs/nfsd 2>/dev/null; then
        $SUDO mount -t nfsd nfsd /proc/fs/nfsd 2>/dev/null ||
            warn "could not mount /proc/fs/nfsd"
    fi
    $SUDO rpc.nfsd 8 || die "rpc.nfsd failed -- is the container --privileged?"
    $SUDO exportfs -ra || die "exportfs failed"
    pidof rpc.mountd >/dev/null 2>&1 ||
        $SUDO rpc.mountd || die "rpc.mountd failed"
fi

log "applying exports"
$SUDO exportfs -ra
$SUDO exportfs -v

# Prove the export serves before handing over. xfstests reports an unusable
# TEST_DEV as a confusing _notrun much later, or as a mount failure with no
# hint of which side is at fault.
log "verifying the export mounts and is writable"
probe="$(mktemp -d)"
if ! $SUDO mount -t nfs -o "vers=${NFS_LOOPBACK_VERS}" \
        "127.0.0.1:${NFS_LOOPBACK_BASE}/test" "$probe"; then
    rmdir "$probe"
    die "cannot mount the test export -- see 'exportfs -v' and 'journalctl -u nfs-server'"
fi
if ! $SUDO touch "${probe}/.probe" 2>/dev/null; then
    $SUDO umount "$probe"; rmdir "$probe"
    die "the export mounted but is not writable"
fi
$SUDO rm -f "${probe}/.probe"
$SUDO umount "$probe"
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

$SUDO mkdir -p "$TEST_MNT" "$SCRATCH_MNT"

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
( cd "$XFSTESTS_DIR" && $SUDO ./check -nfs "${exclude[@]}" "${check_args[@]}" )
status=$?
set -e

if [ "$status" -eq 0 ]; then
    log "xfstests passed"
else
    log "xfstests failed (status $status) -- see ${XFSTESTS_DIR}/results/"
fi
exit "$status"
