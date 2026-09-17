#!/bin/bash
# Fetch the third-party sources this test framework builds against:
# the VAST NFS source tarball, and the upstream Linux subtrees it forks.
#
# Everything lands in $SRC_DIR (default: the repository root) and is
# gitignored: ./linux and ./vastnfs-<version>.
# Idempotent: re-running only fetches what is missing or at the wrong revision.
#
# Usage: scripts/fetch-sources.sh [linux|vastnfs]...
#        (no arguments fetches both)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${SRC_DIR:-${REPO_ROOT}}"

# --- pins ---------------------------------------------------------------
# Bump LINUX_REF_PINNED and LINUX_SHA together, deliberately.

# The baseline VAST NFS 4.5.x forks from. Not a guess: vastnfs scripts/BASE
# says v6.12.57, and scripts/sync-from-linux.sh refuses any other checkout.
# It is a stable tag, so mainline (torvalds) does not contain it -- this
# repo's URL is the "stable" tree instead, a superset with both the vX.Y.Z
# tags and a master tracking mainline, so kunit CI's matrix (see
# .github/workflows/kunit.yml) can request either by ref alone.
#
# LINUX_REF is overridable; the SHA pin below is only checked when it's
# still this default, since a moving ref (master) has no fixed SHA.
# fetch_linux() re-fetches and re-checks-out LINUX_REF on every run, even
# against an already-cloned ./linux, so switching it does not require
# `rm -rf ./linux` first.
LINUX_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
LINUX_REF_PINNED="v6.12.57"
LINUX_REF="${LINUX_REF:-$LINUX_REF_PINNED}"
LINUX_SHA="8a243ecde1f6447b8e237f2c1c67c0bb67d16d67"

VASTNFS_VERSION="4.5.8"
VASTNFS_BASE_URL="https://vast-nfs.s3.amazonaws.com"
VASTNFS_SHA256="a4abaf2d6034d2b9d8d42086c30c355b2baf680ee3fb8e53a63af969f32d3b52"
# ------------------------------------------------------------------------

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# The VAST tree ships both Makefile and makefile in the same directories
# (root and each src/v*), so a case-insensitive filesystem (macOS, Windows,
# WSL drvfs mounts of C:) silently drops 6 files on extraction.
warn_if_case_insensitive_fs() {
    local dir="$1" probe lower
    probe="$(mktemp "${dir}/.caseXXXXXX")"
    lower="${dir}/$(basename "$probe" | tr 'A-Z' 'a-z')"
    if [ "$probe" != "$lower" ] && [ -e "$lower" ]; then
        warn "${dir} is on a case-insensitive filesystem. The VAST source ships
         both Makefile and makefile in one directory, so the extracted tree
         will be missing 6 files and will not build. Set SRC_DIR to a
         case-sensitive path to get a complete tree."
    fi
    rm -f "$probe"
}

# Shallow: one commit, not the whole history. `fetch --depth 1 <ref>` takes
# any ref shape (tag, branch, or a bare commit SHA -- e.g. to bisect a fix,
# see docs/kunit-nfs-reference.md) uniformly, unlike `clone --branch`, which
# rejects a bare SHA.
fetch_linux() {
    local dir="${SRC_DIR}/linux"

    if [ ! -d "$dir/.git" ]; then
        log "cloning linux"
        git init --quiet "$dir"
        git -C "$dir" remote add origin "$LINUX_URL"
    fi

    log "checking out linux ${LINUX_REF}"
    git -C "$dir" fetch --quiet --depth 1 origin "$LINUX_REF"
    git -C "$dir" checkout --quiet FETCH_HEAD

    local head_sha
    head_sha="$(git -C "$dir" rev-parse HEAD)"
    if [ "$LINUX_REF" = "$LINUX_REF_PINNED" ]; then
        [ "$head_sha" = "$LINUX_SHA" ] || die "linux: HEAD is not ${LINUX_SHA}"
    elif [ "$LINUX_REF" = "$head_sha" ]; then
        : # An exact commit SHA, not a moving ref -- nothing to warn about.
    else
        warn "linux: ${LINUX_REF} is a floating ref, not the pinned ${LINUX_REF_PINNED} -- not verified"
    fi

    log "linux: ${LINUX_REF} ${head_sha:0:12} ok"
}

# Source tarball only; VAST publishes no public git repository.
fetch_vastnfs() {
    local tarball="vastnfs-${VASTNFS_VERSION}.tar.xz"
    local dest="${SRC_DIR}/${tarball}"
    local repo_copy="${REPO_ROOT}/vastnfs/${tarball}"

    if [ -e "$repo_copy" ]; then
        log "using in-repo ${tarball}"
        dest="$repo_copy"
    elif [ ! -e "$dest" ]; then
        log "downloading ${tarball}"
        curl --proto '=https' --tlsv1.2 -sSf \
            "${VASTNFS_BASE_URL}/version/${VASTNFS_VERSION}/source/${tarball}" \
            -o "$dest"
    fi

    echo "${VASTNFS_SHA256}  ${dest}" | sha256sum -c - >/dev/null ||
        die "${tarball}: sha256 mismatch"

    if [ ! -d "${SRC_DIR}/vastnfs-${VASTNFS_VERSION}" ]; then
        warn_if_case_insensitive_fs "$SRC_DIR"
        log "extracting ${tarball}"
        tar xf "$dest" -C "$SRC_DIR"
    fi

    # Cross-check the pin: the VAST tree names its own upstream baseline.
    local base
    base="$(cat "${SRC_DIR}/vastnfs-${VASTNFS_VERSION}/scripts/BASE")"
    [ "$base" = "$LINUX_REF" ] ||
        die "vastnfs scripts/BASE is ${base}, but LINUX_REF is ${LINUX_REF}"
    log "vastnfs: ${VASTNFS_VERSION} ok, upstream base ${base}"
}

main() {
    mkdir -p "$SRC_DIR"
    local targets=("$@")
    [ ${#targets[@]} -eq 0 ] && targets=(linux vastnfs)

    for t in "${targets[@]}"; do
        case "$t" in
            linux)   fetch_linux ;;
            vastnfs) fetch_vastnfs ;;
            *)       die "unknown target: $t" ;;
        esac
    done

    log "sources are in ${SRC_DIR}"
}

main "$@"
