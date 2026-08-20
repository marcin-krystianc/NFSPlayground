#!/bin/bash
# Fetch all third-party sources this test framework builds against.
#
# Everything lands in $SRC_DIR (default: external/) which is gitignored.
# Idempotent: re-running only fetches what is missing or at the wrong revision.
#
# Usage: scripts/fetch-sources.sh [xfstests|nfs-utils|linux|vastnfs]...
#        (no arguments fetches all of them)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${SRC_DIR:-${REPO_ROOT}/external}"

# --- pins ---------------------------------------------------------------
# Bump these deliberately; every one is verified after checkout.

XFSTESTS_URL="https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git"
XFSTESTS_REF="v2026.07.21"
XFSTESTS_SHA="56c410ad0f69da5b13c5807bc47b4876dcfa02b2"

# git.linux-nfs.org serves an expired TLS certificate; this GitHub mirror of
# steved/nfs-utils resolves to the identical commit.
NFSUTILS_URL="https://github.com/linux-nfs/nfs-utils.git"
NFSUTILS_REF="nfs-utils-2-9-3-rc1"
NFSUTILS_SHA="67ed1bdb1af1c70a5fd3b377a997401b7676b827"

# The baseline VAST NFS 4.5.x forks from. Not a guess: vastnfs scripts/BASE
# says v6.12.57, and scripts/sync-from-linux.sh refuses any other checkout.
# It is a stable tag, so mainline (torvalds) does not contain it.
LINUX_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
LINUX_REF="v6.12.57"
LINUX_SHA="8a243ecde1f6447b8e237f2c1c67c0bb67d16d67"
# Only the subtrees VAST replaces. A full mainline clone is ~8 GB; this is
# ~270 MB of git data and a ~24 MB working tree.
LINUX_PATHS=(fs/nfs fs/nfsd fs/lockd fs/nfs_common
             net/sunrpc include/linux/nfs include/linux/sunrpc)

VASTNFS_VERSION="4.5.8"
VASTNFS_BASE_URL="https://vast-nfs.s3.amazonaws.com"
VASTNFS_SHA256="a4abaf2d6034d2b9d8d42086c30c355b2baf680ee3fb8e53a63af969f32d3b52"
# ------------------------------------------------------------------------

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# The VAST tree ships both Makefile and makefile in the same directories, so
# it cannot be unpacked on a case-insensitive filesystem (macOS, Windows,
# WSL drvfs mounts of C:).
require_case_sensitive_fs() {
    local dir="$1" probe lower
    probe="$(mktemp "${dir}/.caseXXXXXX")"
    lower="${dir}/$(basename "$probe" | tr 'A-Z' 'a-z')"
    if [ "$probe" != "$lower" ] && [ -e "$lower" ]; then
        rm -f "$probe"
        die "${dir} is on a case-insensitive filesystem. The VAST source ships
       both Makefile and makefile in one directory. Set SRC_DIR to a
       case-sensitive path."
    fi
    rm -f "$probe"
}

clone_at() {
    local name="$1" url="$2" ref="$3" sha="$4" dir="${SRC_DIR}/$1"

    if [ ! -d "$dir/.git" ]; then
        log "cloning ${name} ${ref}"
        git clone --quiet "$url" "$dir"
    fi
    if [ "$(git -C "$dir" rev-parse HEAD)" != "$sha" ]; then
        git -C "$dir" cat-file -e "${sha}^{commit}" 2>/dev/null ||
            git -C "$dir" fetch --quiet --tags origin
        log "checking out ${name} ${ref} (${sha:0:12})"
        git -C "$dir" checkout --quiet "$sha"
    fi
    [ "$(git -C "$dir" rev-parse HEAD)" = "$sha" ] ||
        die "${name}: HEAD is not ${sha}"
    log "${name}: ${ref} ${sha:0:12} ok"
}

fetch_xfstests() {
    clone_at xfstests "$XFSTESTS_URL" "$XFSTESTS_REF" "$XFSTESTS_SHA"
}

fetch_nfs_utils() {
    clone_at nfs-utils "$NFSUTILS_URL" "$NFSUTILS_REF" "$NFSUTILS_SHA"
}

# Blobless + shallow + sparse: only the NFS subtrees, at one tag.
fetch_linux() {
    local dir="${SRC_DIR}/linux"

    if [ ! -d "$dir/.git" ]; then
        log "cloning linux ${LINUX_REF} (blobless, sparse)"
        git clone --quiet --filter=blob:none --sparse \
            --depth 1 --branch "$LINUX_REF" "$LINUX_URL" "$dir"
    fi
    git -C "$dir" sparse-checkout set "${LINUX_PATHS[@]}"
    [ "$(git -C "$dir" rev-parse HEAD)" = "$LINUX_SHA" ] ||
        die "linux: HEAD is not ${LINUX_SHA}"
    log "linux: ${LINUX_REF} ${LINUX_SHA:0:12} ok, $(git -C "$dir" sparse-checkout list | wc -l) paths"
}

# Source tarball only; VAST publishes no public git repository.
fetch_vastnfs() {
    local tarball="vastnfs-${VASTNFS_VERSION}.tar.xz"
    local dest="${SRC_DIR}/${tarball}"
    local repo_copy="${REPO_ROOT}/vastnfs/${tarball}"

    if [ ! -e "$dest" ]; then
        if [ -e "$repo_copy" ]; then
            log "using in-repo ${tarball}"
            cp "$repo_copy" "$dest"
        else
            log "downloading ${tarball}"
            curl --proto '=https' --tlsv1.2 -sSf \
                "${VASTNFS_BASE_URL}/version/${VASTNFS_VERSION}/source/${tarball}" \
                -o "$dest"
        fi
    fi

    echo "${VASTNFS_SHA256}  ${dest}" | sha256sum -c - >/dev/null ||
        die "${tarball}: sha256 mismatch"

    if [ ! -d "${SRC_DIR}/vastnfs-${VASTNFS_VERSION}" ]; then
        require_case_sensitive_fs "$SRC_DIR"
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
    [ ${#targets[@]} -eq 0 ] && targets=(xfstests nfs-utils linux vastnfs)

    for t in "${targets[@]}"; do
        case "$t" in
            xfstests)  fetch_xfstests ;;
            nfs-utils) fetch_nfs_utils ;;
            linux)     fetch_linux ;;
            vastnfs)   fetch_vastnfs ;;
            *)         die "unknown target: $t" ;;
        esac
    done

    log "sources are in ${SRC_DIR}"
}

main "$@"
