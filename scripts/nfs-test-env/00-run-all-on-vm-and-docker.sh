#!/bin/bash
# One command: bring up the docker NFS servers, build xfstests if needed, run
# the tests against them, tear the servers down again.
#
# This is a wrapper over the numbered scripts, not a replacement for them --
# 01 brings up the servers, 02 writes local.config, 03 runs check, 99 tears
# down. Run those individually when iterating on one step.
#
# The teardown is in an EXIT trap, so the containers go away whether the tests
# pass, fail, or the run is interrupted. The results in xfstests/results/
# survive teardown, which is what matters for diagnosing a failure.
#
# Usage: scripts/nfs-test-env/run-all.sh                  # the default set
#        scripts/nfs-test-env/run-all.sh generic/001 generic/002
#        scripts/nfs-test-env/run-all.sh -g quick          # 645 tests, hours
#
# Exits with xfstests' own status, so it is usable in CI.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./env.sh

# The sibling scripts are invoked as "bash ./x.sh", not "./x.sh": they are
# mode 644 in git, so relying on the exec bit fails with a bare 126.

# Default matches .github/workflows/xfstests.yml. Not "-g quick": that is 645
# tests for NFS and runs for hours, because over a loopback/docker mount the
# cost is per-test mount and sync latency (generic/007 alone is ~6 minutes).
# attr+acl+dir is 61 tests and still covers xattrs, ACLs and directory ops.
default_args=(-g attr -g acl -g dir)
check_args=("$@")
[ ${#check_args[@]} -gt 0 ] || check_args=("${default_args[@]}")

command -v docker >/dev/null || die "docker not found"

# Fail early with the fix rather than deep inside make. xfstests needs these
# to build; a fresh clone ships ./check (a shell script) but no binaries, so
# without them the first symptom is xfstests' own terse "fsstress not found or
# executable" from a completely different step.
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

teardown() {
    # Preserve the status we are exiting with; the trap must not change it.
    local rc=$?
    log "tearing down the docker servers"
    bash ./99-teardown.sh || warn "teardown reported problems -- check 'docker ps -a'"
    return $rc
}
trap teardown EXIT

bash ./01-setup-servers.sh

# Build only when there is nothing built: make is quick to no-op but the
# check keeps a normal run's output readable.
if [ -x "${XFSTESTS_DIR}/ltp/fsstress" ]; then
    log "xfstests already built"
else
    log "building xfstests"
    make -C "$XFSTESTS_DIR" -j"$(nproc)" >/tmp/xfstests-build.log 2>&1 ||
        die "xfstests build failed -- see /tmp/xfstests-build.log"
fi

bash ./02-configure-xfstests.sh

log "running xfstests: ${check_args[*]}"
status=0
set +e
bash ./03-run-xfstests.sh "${check_args[@]}"
status=$?
set -e

if [ "$status" -eq 0 ]; then
    log "xfstests passed"
else
    log "xfstests failed (status $status) -- see ${XFSTESTS_DIR}/results/"
fi
exit "$status"
