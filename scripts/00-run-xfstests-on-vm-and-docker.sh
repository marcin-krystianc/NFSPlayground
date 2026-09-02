#!/bin/bash
# One self-contained script: bring up the docker NFS servers, build xfstests,
# run the tests against them, tear the servers down again.
#
# Standalone on purpose. It does not source env.sh and does not call the
# numbered scripts -- copy this one file to a machine with docker and a
# checked-out xfstests and it runs. The cost of that is duplication: the
# container layout here must stay in step with 01-setup-servers.sh, the
# local.config with 02-configure-xfstests.sh, and the cleanup with
# 99-teardown.sh. Change one, check the other.
#
# The teardown is in an EXIT trap, so the containers go away whether the
# tests pass, fail, or the run is interrupted. xfstests/results/ survives,
# which is what matters for diagnosing a failure.
#
# Usage: bash scripts/00-run-xfstests-on-vm-and-docker.sh
#        bash scripts/00-run-xfstests-on-vm-and-docker.sh generic/001 generic/002
#        bash scripts/00-run-xfstests-on-vm-and-docker.sh -g quick
#
# Exits with xfstests' own status.

set -euo pipefail

# ---------------------------------------------------------------------------
# Config. Every value is overridable from the environment; the defaults match
# scripts/nfs-test-env/env.sh so this script and the numbered ones can be
# used interchangeably on the same machine.
# ---------------------------------------------------------------------------
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
XFSTESTS_DIR="${XFSTESTS_DIR:-${REPO_ROOT}/xfstests}"

NFS_NET="${NFS_NET:-nfstestnet}"
NFS_SUBNET="${NFS_SUBNET:-172.28.0.0/24}"
NFS_IP_PREFIX="${NFS_IP_PREFIX:-172.28.0.1}"   # server $i gets ${NFS_IP_PREFIX}$i
NFS_SERVER_COUNT="${NFS_SERVER_COUNT:-3}"      # >=2: index 1 is TEST_DEV, 2 is SCRATCH_DEV
NFS_SERVER_IMAGE="${NFS_SERVER_IMAGE:-erichough/nfs-server}"
NFS_EXPORT_BASE="${NFS_EXPORT_BASE:-${HOME}/nfs-test-env-exports}"

# Identical fsid across servers makes a filehandle from one node valid on
# another (what remoteports= needs); distinct between test and scratch so
# xfstests still sees two filesystems.
NFS_TEST_FSID="${NFS_TEST_FSID:-1}"
NFS_SCRATCH_FSID="${NFS_SCRATCH_FSID:-2}"

TEST_MNT="${TEST_MNT:-/mnt/nfs-test-env/test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/nfs-test-env/scratch}"

# Not "-g quick": that is 645 tests for NFS and runs for hours, because the
# cost is per-test mount and sync latency (generic/007 alone is ~6 minutes).
# attr+acl+dir is 61 tests covering xattrs, ACLs and directory operations.
# Note it is a different slice, not a subset -- it includes tests quick omits.
DEFAULT_CHECK_ARGS=( -g quick)

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

server_ip()   { echo "${NFS_IP_PREFIX}$1"; }
server_name() { echo "nfs-test-env-$1"; }

check_args=("$@")
[ ${#check_args[@]} -gt 0 ] || check_args=("${DEFAULT_CHECK_ARGS[@]}")

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
command -v docker >/dev/null || die "docker not found"
[ "$NFS_SERVER_COUNT" -ge 2 ] ||
    die "need NFS_SERVER_COUNT >= 2 (one for TEST_DEV, one for SCRATCH_DEV)"
[ -f "${XFSTESTS_DIR}/check" ] ||
    die "no xfstests at ${XFSTESTS_DIR} -- git submodule update --init xfstests"

# Fail here with the fix rather than deep inside make. A fresh xfstests clone
# ships ./check (a shell script) but no compiled binaries, so without these
# the first symptom is xfstests' own terse "fsstress not found or executable"
# from a much later step.
if [ ! -x "${XFSTESTS_DIR}/ltp/fsstress" ]; then
    missing=()
    command -v gcc  >/dev/null || missing+=(build-essential)
    command -v make >/dev/null || missing+=(build-essential)
    [ -e /usr/include/attr/attributes.h ] || missing+=(libattr1-dev)
    [ -e /usr/include/sys/acl.h ]         || missing+=(libacl1-dev)
    [ -e /usr/include/libaio.h ]          || missing+=(libaio-dev)
    [ -e /usr/include/uuid/uuid.h ]       || missing+=(uuid-dev)
    if [ ${#missing[@]} -gt 0 ]; then
        die "missing xfstests build dependencies. Install them with:
    sudo apt-get install -y build-essential autoconf automake libtool-bin \\
        pkg-config gettext uuid-dev libattr1-dev libacl1-dev libaio-dev \\
        libgdbm-dev xfslibs-dev xfsprogs e2fsprogs attr acl quota"
    fi
fi

# ---------------------------------------------------------------------------
# Teardown, registered before anything is created so an early failure still
# cleans up. Must not change the status we are exiting with.
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
    for i in $(seq 1 "$NFS_SERVER_COUNT"); do
        name="$(server_name "$i")"
        if docker inspect "$name" >/dev/null 2>&1; then
            docker rm -f "$name" >/dev/null 2>&1 || warn "could not remove $name"
        fi
    done
    if docker network inspect "$NFS_NET" >/dev/null 2>&1; then
        docker network rm "$NFS_NET" >/dev/null 2>&1 ||
            warn "could not remove network $NFS_NET"
    fi
    return $rc
}
trap teardown EXIT

# ---------------------------------------------------------------------------
# Servers
# ---------------------------------------------------------------------------
if docker network inspect "$NFS_NET" >/dev/null 2>&1; then
    log "docker network $NFS_NET already exists"
else
    log "creating docker network $NFS_NET ($NFS_SUBNET)"
    docker network create --subnet "$NFS_SUBNET" "$NFS_NET" >/dev/null
fi

# Shared backing store, exported by every node. The root is bind-mounted (not
# just its children) so the NFSv4 pseudo-root filehandle is identical across
# nodes too -- a container-local root would have a different inode per
# container and break v4 multipath at the root.
mkdir -p "${NFS_EXPORT_BASE}/root/test" "${NFS_EXPORT_BASE}/root/scratch"
chmod 777 "${NFS_EXPORT_BASE}/root" \
          "${NFS_EXPORT_BASE}/root/test" "${NFS_EXPORT_BASE}/root/scratch"

for i in $(seq 1 "$NFS_SERVER_COUNT"); do
    name="$(server_name "$i")"
    ip="$(server_ip "$i")"

    if docker inspect "$name" >/dev/null 2>&1; then
        # Existing is not the same as running: a host reboot or an
        # interrupted run leaves the container stopped, and treating that as
        # "already there" is what surfaces later as "No route to host" from
        # mount.nfs, a long way from the cause.
        if [ "$(docker inspect -f '{{.State.Running}}' "$name")" = "true" ]; then
            log "$name already running"
        else
            log "$name exists but is stopped, starting it"
            docker start "$name" >/dev/null
        fi
        continue
    fi

    log "starting $name at $ip"
    docker run -d --name "$name" --privileged --network "$NFS_NET" --ip "$ip" \
        -v "${NFS_EXPORT_BASE}/root:/export" \
        -e NFS_EXPORT_0="/export *(rw,fsid=0,no_subtree_check,insecure,no_root_squash,crossmnt)" \
        -e NFS_EXPORT_1="/export/test *(rw,fsid=${NFS_TEST_FSID},no_subtree_check,insecure,no_root_squash)" \
        -e NFS_EXPORT_2="/export/scratch *(rw,fsid=${NFS_SCRATCH_FSID},no_subtree_check,insecure,no_root_squash)" \
        "$NFS_SERVER_IMAGE" >/dev/null
done

# nfsd inside a freshly started container takes a moment to serve. Without
# this the first mount can race the server and fail with "No route to host"
# or a connection refusal -- the numbered scripts got away with it because a
# human paused between steps.
log "waiting for the servers to answer"
for i in 1 2; do
    ip="$(server_ip "$i")"
    for attempt in $(seq 1 30); do
        if timeout 3 showmount -e "$ip" >/dev/null 2>&1; then
            echo "  $ip ready"
            break
        fi
        [ "$attempt" -lt 30 ] || die "$ip never answered -- 'docker logs $(server_name "$i")'"
        sleep 1
    done
done

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

# TEST_DEV and SCRATCH_DEV name different servers, which only decides where
# traffic goes absent remoteports=; every node serves both exports.
config="${XFSTESTS_DIR}/local.config"
log "writing $config"
cat > "$config" <<EOF
# generated by 00-run-xfstests-on-vm-and-docker.sh -- do not edit, it is overwritten
export FSTYP=nfs
export TEST_DEV=$(server_ip 1):/export/test
export TEST_DIR=${TEST_MNT}
export SCRATCH_DEV=$(server_ip 2):/export/scratch
export SCRATCH_MNT=${SCRATCH_MNT}
# VAST multipath range for xfstests/tests/nfs/002 -- every server, not just
# the two named above.
export NFS_MULTIPATH_REMOTEPORTS=$(server_ip 1)-$(server_ip "$NFS_SERVER_COUNT")
EOF
cat "$config"

log "running xfstests: ${check_args[*]}"
status=0
set +e
( cd "$XFSTESTS_DIR" && sudo ./check -nfs "${check_args[@]}" )
status=$?
set -e

if [ "$status" -eq 0 ]; then
    log "xfstests passed"
else
    log "xfstests failed (status $status) -- see ${XFSTESTS_DIR}/results/"
fi
exit "$status"
