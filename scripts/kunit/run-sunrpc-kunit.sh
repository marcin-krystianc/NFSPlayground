#!/bin/bash
# Run the SunRPC KUnit suites under UML.
#
# kunit.py needs a complete kernel tree, which the default sparse checkout
# is not. Fetch one first with:
#
#     LINUX_FULL=1 scripts/fetch-sources.sh linux
#
# ./linux is gitignored, so kunit/addr_test.c lives in this repo and is
# copied into the tree here, along with the Kconfig/Makefile/.kunitconfig
# wiring. Every edit is grep-guarded, so re-running is safe and a
# re-fetched tree is re-wired automatically.
#
# Usage: scripts/kunit/run-sunrpc-kunit.sh [extra kunit.py args...]
#        scripts/kunit/run-sunrpc-kunit.sh --raw_output

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINUX_DIR="${LINUX_DIR:-${REPO_ROOT}/linux}"
SUNRPC_DIR="${LINUX_DIR}/net/sunrpc"

log()  { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -f "${LINUX_DIR}/Makefile" ] || die "${LINUX_DIR} is not a kernel tree"
[ -f "${LINUX_DIR}/init/main.c" ] ||
    die "${LINUX_DIR} looks sparse; run: LINUX_FULL=1 scripts/fetch-sources.sh linux"
[ -x "${LINUX_DIR}/tools/testing/kunit/kunit.py" ] ||
    die "kunit.py missing from ${LINUX_DIR}/tools/testing/kunit"

# Each entry is "stem:subdir:Kconfig symbol:Kconfig depends:description".
# Adding a suite means dropping the .c in kunit/ and adding a line here.
TESTS=(
    "addr_test:net/sunrpc:SUNRPC_ADDR_KUNIT_TEST:SUNRPC:SunRPC address conversion"
    "timer_test:net/sunrpc:SUNRPC_TIMER_KUNIT_TEST:SUNRPC:SunRPC RTT estimator"
    "nfs_common_test:fs/nfs_common:NFS_COMMON_KUNIT_TEST:NFS_COMMON:NFS status to errno translation"
    "xdr_test:net/sunrpc:SUNRPC_XDR_KUNIT_TEST:SUNRPC:SunRPC XDR codec"
    "nfs4session_test:fs/nfs:NFS_V4_SESSION_KUNIT_TEST:NFS_V4:NFSv4.1 session slot tables"
    "inode_test:fs/nfs:NFS_INODE_KUNIT_TEST:NFS_FS:NFS inode attribute comparison"
    "pnfs_test:fs/nfs:NFS_PNFS_KUNIT_TEST:NFS_V4_1:pNFS layout range arithmetic"
    "pagelist_test:fs/nfs:NFS_PAGELIST_KUNIT_TEST:NFS_FS:NFS page request coalescing"
    "nfs4proc_test:fs/nfs:NFS_V4_PROC_KUNIT_TEST:NFS_V4:NFSv4 protocol decision logic"
    "xfstests/generic/001:fs:NFS_GENERIC001_KUNIT_TEST:NFSD:xfstests generic/001 over a loopback NFS mount"
)

# NFS_V4 is needed by the session slot table suite and is not in the
# stock .kunitconfig, which only enables CONFIG_NFS_FS.
# NFS_V4_2/NFSD/TMPFS serve the generic/001 suite, which stands up knfsd
# inside the UML kernel and mounts it back over loopback; the export lives
# on tmpfs because ramfs has no export_operations.
kunit_opts=(CONFIG_IPV6=y CONFIG_NFS_V4=y CONFIG_NFS_V4_1=y
            CONFIG_NFS_V4_2=y CONFIG_NFSD=y CONFIG_NFSD_V4=y CONFIG_TMPFS=y)

# Some functions worth testing are file-private. The kernel's own answer to
# that is VISIBLE_IF_KUNIT (include/kunit/visibility.h), which drops the
# `static` only when CONFIG_KUNIT is set, paired with
# EXPORT_SYMBOL_IF_KUNIT to put the symbol in a test-only namespace. That
# requires editing the file under test, so it is done here rather than in
# the test source. Each entry is "file:function".
# Entries are "file:return type:function".
UNSTATIC=(
    "fs/nfs/inode.c:int:nfs_inode_attrs_cmp"
    "fs/nfs/inode.c:int:nfs_attribute_timeout"
    "fs/nfs/inode.c:bool:nfs_check_cache_flags_invalid"
    "fs/nfs/inode.c:void:nfs_ooo_merge"
    "fs/nfs/inode.c:void:nfs_zap_caches_locked"
    "fs/nfs/inode.c:u32:nfs_get_valid_attrmask"
    "fs/nfs/inode.c:bool:nfs_file_has_writers"
    "fs/nfs/inode.c:int:nfs_update_inode"
    "fs/nfs/inode.c:void:nfs_wcc_update_inode"
    "fs/nfs/inode.c:int:nfs_check_inode_attributes"
    "fs/nfs/inode.c:void:nfs_update_timestamps"
    "fs/nfs/inode.c:int:nfs_find_actor"
    "fs/nfs/inode.c:int:nfs_init_locked"
    "fs/nfs/inode.c:bool:nfs_getattr_readdirplus_enable"
    "fs/nfs/inode.c:int:__nfs_revalidate_inode"
    "fs/nfs/inode.c:void:nfs_inode_init_regular"
    "fs/nfs/inode.c:void:nfs_inode_init_dir"
    "fs/nfs/inode.c:struct nfs_lock_context *:__nfs_find_lock_context"
    "fs/nfs/inode.c:bool:nfs_file_has_buffered_writers"
    "fs/nfs/inode.c:void:nfs_fattr_fixup_delegated"
    "fs/nfs/inode.c:void:nfs_init_lock_context"
    "fs/nfs/inode.c:int:nfs_vmtruncate"
    "fs/nfs/inode.c:void:init_once"
    "fs/nfs/inode.c:int:nfs_invalidate_mapping"
    "fs/nfs/inode.c:void:nfs_set_timestamps_to_ts"
    "fs/nfs/inode.c:void:nfs_ooo_record"
    "fs/nfs/inode.c:int:nfs_inode_finish_partial_attr_update"
    "fs/nfs/pagelist.c:bool:nfs_page_is_contiguous"
    "fs/nfs/pagelist.c:bool:nfs_match_lock_context"
    "fs/nfs/pagelist.c:unsigned int:nfs_coalesce_size"
    "fs/nfs/nfs4proc.c:int:nfs4_map_errors"
    "fs/nfs/nfs4proc.c:long:nfs4_update_delay"
    "fs/nfs/nfs4proc.c:u32:nfs4_fmode_to_share_access"
    "fs/nfs/nfs4proc.c:u32:nfs4_map_atomic_open_share"
    "fs/nfs/nfs4proc.c:enum open_claim_type4:nfs4_map_atomic_open_claim"
    "fs/nfs/nfs4proc.c:const nfs4_stateid *:nfs4_recoverable_stateid"
    "fs/nfs/nfs4proc.c:void:nfs4_bitmap_copy_adjust"
    "fs/nfs/nfs4proc.c:void:nfs4_slot_sequence_record_sent"
    "fs/nfs/nfs4proc.c:void:nfs4_slot_sequence_acked"
    "fs/nfs/nfs4proc.c:fmode_t:_nfs4_ctx_to_accessmode"
    "fs/nfs/nfs4proc.c:fmode_t:_nfs4_ctx_to_openmode"
    "fs/nfs/nfs4proc.c:bool:nfs4_server_supports_acls"
    "fs/nfs/nfs4proc.c:ssize_t:nfs4_proc_get_acl"
    "fs/nfs/nfs4proc.c:int:nfs4_proc_set_acl"
    "fs/nfs/nfs4proc.c:int:__nfs4_proc_set_acl"
    "fs/nfs/nfs4proc.c:void:nfs4_update_changeattr_locked"
    "fs/nfs/nfs4proc.c:int:nfs4_proc_unlink_done"
    "fs/nfs/nfs4proc.c:void:nfs4_fattr_set_prechange"
    "fs/nfs/nfs4proc.c:int:nfs4_exception_should_retrans"
    "fs/nfs/nfs4proc.c:bool:_nfs4_is_integrity_protected"
    "fs/nfs/nfs4proc.c:int:nfs4_sequence_process"
    "fs/nfs/nfs4proc.c:void:nfs4_sequence_free_slot"
    "fs/nfs/nfs4proc.c:bool:nfs4_clear_cap_atomic_open_v1"
    "fs/nfs/nfs4proc.c:bool:nfs4_mode_match_open_stateid"
    "fs/nfs/nfs4proc.c:int:can_open_cached"
    "fs/nfs/nfs4proc.c:int:can_open_delegated"
    "fs/nfs/nfs4proc.c:void:update_open_stateflags"
    "fs/nfs/nfs4proc.c:bool:nfs_open_stateid_recover_openmode"
    "fs/nfs/nfs4proc.c:void:nfs_state_log_update_open_stateid"
    "fs/nfs/nfs4proc.c:void:nfs_resync_open_stateid_locked"
    "fs/nfs/nfs4proc.c:void:nfs_clear_open_stateid_locked"
    "fs/nfs/nfs4proc.c:void:nfs_state_clear_open_state_flags"
    "fs/nfs/nfs4proc.c:void:nfs_state_set_delegation"
    "fs/nfs/nfs4proc.c:void:nfs_state_clear_delegation"
    "fs/nfs/nfs4proc.c:int:nfs4_check_cl_exchange_flags"
    "fs/nfs/nfs4proc.c:bool:nfs41_same_server_scope"
    "fs/nfs/nfs4proc.c:int:nfs4_verify_fore_channel_attrs"
    "fs/nfs/nfs4proc.c:int:nfs4_verify_back_channel_attrs"
    "fs/nfs/nfs4proc.c:int:nfs4_verify_channel_attrs"
    "fs/nfs/nfs4proc.c:bool:nfs4_match_stateid"
    "fs/nfs/nfs4proc.c:bool:nfs41_match_stateid"
    "fs/nfs/nfs4proc.c:bool:nfs4_error_stateid_expired"
    "fs/nfs/nfs4proc.c:bool:_is_same_nfs4_pathname"
    "fs/nfs/nfs4proc.c:void:nfs4_sequence_attach_slot"
    "fs/nfs/nfs4proc.c:void:nfs4_inc_nlink_locked"
    "fs/nfs/nfs4proc.c:void:nfs4_inc_nlink"
    "fs/nfs/nfs4proc.c:void:nfs4_dec_nlink_locked"
    "fs/nfs/nfs4proc.c:void:nfs4_update_changeattr"
    "fs/nfs/nfs4proc.c:bool:nfs_stateid_is_sequential"
    "fs/nfs/nfs4proc.c:void:nfs_clear_open_stateid"
    "fs/nfs/nfs4proc.c:void:nfs4_return_incompatible_delegation"
    "fs/nfs/nfs4proc.c:void:nfs4_close_context"
    "fs/nfs/nfs4proc.c:bool:nfs4_read_plus_not_supported"
    "fs/nfs/nfs4proc.c:bool:nfs4_write_need_cache_consistency_data"
    "fs/nfs/nfs4proc.c:void:nfs4_bitmask_set"
    "fs/nfs/nfs4proc.c:int:nfs4_buf_to_pages_noslab"
    "fs/nfs/nfs4proc.c:void:nfs4_zap_acl_attr"
    "fs/nfs/nfs4proc.c:void:nfs_fixup_secinfo_attributes"
    "fs/nfs/nfs4proc.c:void:nfs4_disable_swap"
    "fs/nfs/nfs4proc.c:void:nfs4_init_boot_verifier"
    "fs/nfs/nfs4proc.c:void:do_renew_lease"
    "fs/nfs/nfs4proc.c:void:renew_lease"
    "fs/nfsd/export.c:int:expkey_parse"
    "fs/nfsd/export.c:int:svc_export_parse"
    "net/sunrpc/svcauth_unix.c:int:ip_map_parse"
)

for entry in "${UNSTATIC[@]}"; do
    IFS=: read -r relpath rettype func <<< "$entry"
    src="${LINUX_DIR}/${relpath}"

    [ -f "$src" ] || die "${src} not found"
    grep -q "EXPORT_SYMBOL_IF_KUNIT(${func})" "$src" && continue

    log "exposing ${func}() in ${relpath} for testing"

    grep -q '#include <kunit/visibility.h>' "$src" ||
        sed -i "0,/^#include/s|^#include|#include <kunit/visibility.h>\n#include|" "$src"

    # Drop the `static` on the definition only, not on forward decls.
    # Three layouts occur in this code: "static int foo(" on one line,
    # "static int" with "foo(" on the next, and "static struct x *foo("
    # where the pointer star abuts the name. \s* covers all three, since
    # \s matches the newline too.
    perl -0pi -e "s/^static\s+\Q${rettype}\E\s*\Q${func}\E\(/VISIBLE_IF_KUNIT ${rettype} ${func}(/mg" "$src"

    # Verify by absence: the definition must no longer be static.
    grep -qE "^static .*\b${func}\(" "$src" &&
        die "could not un-static ${func}() in ${relpath}"

    printf '\nEXPORT_SYMBOL_IF_KUNIT(%s);\n' "$func" >> "$src"
done

for entry in "${TESTS[@]}"; do
    IFS=: read -r stem subdir symbol depends description <<< "$entry"
    dir="${LINUX_DIR}/${subdir}"

    [ -d "$dir" ] || die "${dir} not found in the kernel tree"

    # A stem with slashes mirrors a source-tree layout under kunit/ (e.g.
    # xfstests/generic/001). The kernel-side copy flattens it: kbuild would
    # need a Makefile in every subdirectory otherwise.
    flat="${stem//\//_}"
    log "installing ${subdir}/${flat}.c"
    cp "${REPO_ROOT}/kunit/${stem}.c" "${dir}/${flat}.c"

    # Mirrors net/sunrpc/auth_gss/Makefile:17, the pre-existing KUnit test.
    if ! grep -q "$symbol" "${dir}/Makefile"; then
        log "wiring Makefile for $symbol"
        printf '\nobj-$(CONFIG_%s) += %s.o\n' "$symbol" "$flat" \
            >> "${dir}/Makefile"
    fi

    # Mirrors the RPCSEC_GSS_KRB5_KUNIT_TEST stanza in net/sunrpc/Kconfig.
    # fs/nfs_common has no Kconfig of its own, so the stanza goes into the
    # nearest one that is always sourced.
    kconfig="${dir}/Kconfig"
    [ -f "$kconfig" ] || kconfig="${LINUX_DIR}/fs/Kconfig"

    if ! grep -q "$symbol" "$kconfig"; then
        log "wiring $(basename "$(dirname "$kconfig")")/Kconfig for $symbol"
        cat >> "$kconfig" <<EOF

config ${symbol}
	tristate "KUnit tests for ${description}" if !KUNIT_ALL_TESTS
	depends on ${depends} && KUNIT
	default KUNIT_ALL_TESTS
	help
	  This builds the KUnit tests for ${description}.

	  For more information on KUnit and unit tests in general, refer
	  to the KUnit documentation in Documentation/dev-tools/kunit/.
EOF
    fi

    kunit_opts+=("CONFIG_${symbol}=y")
done

# CONFIG_IPV6 is not in the stock .kunitconfig; without it rpc_pton6() and
# rpc_ntop6() compile to stubs and every IPv6 case would vacuously pass.
for opt in "${kunit_opts[@]}"; do
    if ! grep -qx "$opt" "${SUNRPC_DIR}/.kunitconfig"; then
        log "adding $opt to .kunitconfig"
        echo "$opt" >> "${SUNRPC_DIR}/.kunitconfig"
    fi
done

log "running kunit.py"
cd "$LINUX_DIR"
exec ./tools/testing/kunit/kunit.py run \
    --kunitconfig=net/sunrpc/.kunitconfig "$@"
