#!/bin/bash
# Fetch and verify a VAST NFS source tarball.
#
# The tarball is kept here rather than an extracted tree because the source
# contains both Makefile and makefile in the same directories (root and
# src/v*), which cannot be checked out on a case-insensitive filesystem.
#
# Usage: ./fetch.sh [version]   (default: the pinned version below)

set -euo pipefail

VERSION="${1:-4.5.8}"
BASE_URL="https://vast-nfs.s3.amazonaws.com"
cd "$(dirname "$0")"

tarball="vastnfs-${VERSION}.tar.xz"
if [ ! -e "$tarball" ]; then
    curl --proto '=https' --tlsv1.2 -sSf \
        "${BASE_URL}/version/${VERSION}/source/${tarball}" -o "$tarball"
fi

grep " ${tarball}\$" sha256sums.txt | sha256sum -c -

# Latest published versions per branch: curl -sSf ${BASE_URL}/meta.json
