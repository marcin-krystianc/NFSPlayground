#!/usr/bin/env python3
"""Account for every xfstests generic/* case: ported, or skipped with a reason.

Reads xfstests/tests/generic/NNN and kunit/xfstests/generic/NNN.c and writes
docs/xfstests-ports-not-done.md, which lists the cases that are not ported
grouped by why. Most reasons come from a rule below -- a _require call, a
_begin_fstest group, or something in the test body that decides it. The rest
were read one at a time and are listed in MANUAL.

    python3 scripts/kunit/xfstests-port-status.py

Rerun it after adding a port; the doc's counts come from the directory.
"""
import os
import re
import textwrap
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTS = os.path.join(REPO, "xfstests/tests/generic")
PORTED_DIR = os.path.join(REPO, "kunit/xfstests/generic")
OUT = os.path.join(REPO, "docs/xfstests-ports-not-done.md")

# (regex on the whole _require line, reason) -- first match wins.
SKIP_RULES = [
    (r"_require_atime|_require_relatime|_require_noatime",
     "atime mount options have no effect on NFS: upstream _require_atime notruns"),
    (r"_require_acls|_require_acl_get_max|_require_chacl",
     "POSIX ACLs are NFSv3-only in the Linux client (nfs3proc.c wires .set_acl); the fixture mounts v4.2"),
    (r"_require_renameat2",
     "nfs_rename() rejects every RENAME_* flag with -EINVAL (fs/nfs/dir.c)"),
    (r"_require_attrs\s+(\S+\s+)*(trusted|security)|_require_attr_v1|ATTR_MODES",
     "trusted/security xattr namespaces: the NFSv4.2 client carries only user.* (RFC 8276)"),
    (r"_require_dax|_require_scratch_dax", "DAX"),
    (r"_require_dm_target", "device-mapper target: the fixture has no block device"),
    (r"_require_scratch_reflink|_require_test_reflink|_require_cp_reflink|_require_scratch_dedupe|_require_test_dedupe",
     "reflink/dedupe: not an NFS operation"),
    (r"_require_scratch_shutdown|_require_scratch_nocheck_shutdown|godown",
     "filesystem shutdown: not an NFS operation"),
    (r"_require_quota|_require_prjquota|_require_xfs_quota_foreign",
     "quota: not an NFS operation"),
    (r"_require_scratch_encryption|_require_encryption",
     "fscrypt: not an NFS operation"),
    (r"_require_scratch_verity|_require_verity", "fsverity: not an NFS operation"),
    (r"_require_scratch_swapfile|_require_swapfile", "swapfile on NFS is not supported"),
    (r"_require_metadata_journaling", "journalling: the fixture cannot crash and replay"),
    (r"_require_chattr|_require_chprojid", "FS_IOC_SETFLAGS ioctl: not an NFS operation"),
    (r"_require_xfs_io_command\s+\"?fiemap", "fiemap: not an NFS operation"),
    (r"_require_xfs_io_command\s+\"?(falloc -k|fzero|fcollapse|finsert|funshare|zero)",
     "fallocate mode unsupported over NFSv4.2 (only ALLOCATE/DEALLOCATE exist)"),
    # fpunch is not in the list above: PUNCH_HOLE|KEEP_SIZE is DEALLOCATE,
    # which nfs42_fallocate() accepts.
    (r"_require_xfs_io_command\s+\"?(chattr|label|scrub|repair|bulkstat|fsmap|inject|resblks|parent|utimes|syncfs|lsattr)",
     "xfs_io command with no NFS/VFS equivalent"),
    (r"_require_aio|_require_aiodio",
     "libaio: the test's program is linked with libaio. xfs_run_prog() can run userspace programs inside the test kernel, so a static build could run it; not ported yet"),
    (r"_require_fio", "fio: userspace workload generator"),
    (r"_require_freeze", "filesystem freeze: not an NFS operation"),
    (r"_require_attrs\s+trusted|_require_attr_v1", "trusted xattr namespace: not carried over NFS"),
    (r"_require_scratch_size_nocheck|_require_scratch_size|_scratch_mkfs_sized",
     "needs a filesystem of a chosen size (mkfs): no NFS equivalent"),
    (r"_require_loop", "loop device"),
    (r"_require_block_device|_require_local_device|_require_scsi_debug", "needs a real block device"),
    (r"_require_exportfs|_require_ext4_mkfs|_require_xfs_mkfs|_require_btrfs",
     "filesystem-specific tooling"),
    (r"_require_ioctl|_require_fssum|_require_fscrypt|_require_fsverity",
     "ioctl with no NFS equivalent"),
    (r"_require_deletable_subvols|_require_btrfs_command|_require_scratch_btrfs",
     "btrfs-specific"),
    (r"_require_ugid_map|_require_userns", "user namespaces"),
    (r"_require_idmapped_mounts", "idmapped mounts"),
    (r"_require_shutdown", "filesystem shutdown: not an NFS operation"),
    (r"_require_richacl", "richacl"),
    (r"_require_casefold|_require_scratch_casefold", "casefolding: not an NFS operation"),
    (r"_require_atomic_write|_require_scratch_write_atomic", "atomic writes"),
    (r"_require_sparse_files_fibmap|_require_fibmap", "FIBMAP ioctl"),
    (r"_require_seek_data_hole_sparse", "sparse SEEK semantics vary"),
]

# Groups that imply the test is out of reach whatever its _requires say.
SKIP_GROUPS = {
    "aio": "libaio: the test's program is linked with libaio. xfs_run_prog() can run userspace programs inside the test kernel, so a static build could run it; not ported yet",
    "acl": "POSIX ACLs are NFSv3-only in the Linux client; the fixture mounts v4.2",
    "atime": "atime mount options have no effect on NFS: upstream _require_atime notruns",
    "dax": "DAX",
    "dedupe": "reflink/dedupe: not an NFS operation",
    "fiexchange": "FIEXCHANGE_RANGE (exchangerange) ioctl: not an NFS operation",
    "swapext": "the XFS swapext ioctl: not an NFS operation",
    "io_uring": "io_uring: the test's program is linked with liburing. xfs_run_prog() could run a static build; not ported yet",
    "unlink": "O_TMPFILE: fs/nfs wires no .tmpfile inode operation",
    "pipe": "splice to and from pipes across processes",
    "clone": "reflink/clone: not an NFS operation",
    "shutdown": "filesystem shutdown: not an NFS operation",
    "quota": "quota: not an NFS operation",
    "encrypt": "fscrypt: not an NFS operation",
    "verity": "fsverity: not an NFS operation",
    "swap": "swapfile on NFS is not supported",
    "log": "log replay: the fixture cannot crash and replay",
    "recoveryloop": "crash recovery: the fixture cannot crash and replay",
    "fsck": "fsck: no NFS equivalent",
    "defrag": "defrag ioctl: no NFS equivalent",
    "realtime": "XFS realtime device",
    "growfs": "growfs: no NFS equivalent",
    "resize": "resize: no NFS equivalent",
    "scrub": "scrub: no NFS equivalent",
    "trim": "discard/trim: no NFS equivalent",
    "atomicwrites": "atomic writes",
    "idmapped": "idmapped mounts",
    "fiemap": "fiemap: not an NFS operation",
    "thin": "dm-thin",
    "fuzzers": "image fuzzing",
    "exchange": "RENAME_EXCHANGE ioctl paths / not an NFS operation",

}

# Matched against the whole test body, not just its _require lines.
BODY_RULES = [
    (r"^_exclude_fs nfs", "upstream excludes NFS from this test (_exclude_fs nfs)"),
    (r'_require_xfs_io_command\s+"falloc"\s+"-k"',
     "fallocate mode unsupported over NFSv4.2 (only ALLOCATE/DEALLOCATE exist)"),
    (r"_scratch_mkfs_sized|_try_scratch_mkfs_sized|_scratch_mkfs_geom|_scratch_mkfs_blocksized",
     "mkfs of a sized/geometried filesystem: upstream notruns on NFS "
     "(_scratch_mkfs_sized: \"Filesystem nfs not supported\")"),
    (r"_run_fsstress|\bfsstress\b|\$FSX_PROG|run_fsx",
     "driven by ltp/fsstress, a userspace random-operation generator. It "
     "could run inside the test kernel through xfs_run_prog() as fsx does "
     "for 091/127/263/363; not ported yet"),
    (r"src/t_stripealign|_scratch_resvblks|_xfs_force_bdev", "XFS geometry tooling"),
    (r"_scratch_dev_pool|_require_scratch_dev_pool", "needs a pool of block devices"),
    (r"_require_scratch_delalloc",
     "upstream's _require_scratch_delalloc notruns: it needs filefrag to report "
     "a delayed-allocation extent, and NFS has neither"),
    (r"_require_io_uring", "io_uring: the test's program is linked with liburing. xfs_run_prog() could run a static build; not ported yet"),
    (r"_require_scratch_extsize|_require_extsize", "the XFS extent-size hint ioctl"),
    (r'_require_xfs_io_command\s+"?(-T|flink)',
     "O_TMPFILE: fs/nfs wires no .tmpfile inode operation, so the client cannot "
     "create one"),
    (r'_require_command\s+"\$CHATTR_PROG"', "chattr: FS_IOC_SETFLAGS has no NFS equivalent"),
    (r"_require_fs_space\s+\S+\s+\$\(\(4 \* 1024 \* 1024\)\)|_require_fs_space\s+\S+\s+\$\(\(5 \* 1024 \* 1024\)\)",
     "needs several GiB of real space; the fixture's export is a 64 MiB tmpfs"),
]


# Hand-examined tests whose reason no mechanical rule states. Each one was
# read; the reason names what specifically cannot be reproduced.
MANUAL = {
    "010": "src/dbtest drives ndbm, a userspace library, not the filesystem",
    "128": "executes a setuid binary from a nosuid mount as another user. xfs_run_prog() can execute static binaries, from the host directory; running one from the NFS mount under another uid is not done yet",
    "241": "dbench, a userspace workload generator",
    "345": "holetest -F: generic/340 and 344 with processes instead of threads. In a kernel test both markers are kthreads sharing one mm, so the port would duplicate generic/340",
    "445": "seek_sanity_test case 17 skips itself unless the page size is at least four allocation units; the probe finds 4096 on the tmpfs export (logged by the generic/436 port), so upstream reports it skipped",
    "460": "the bug is XFS's delalloc indirect-block reservation, reached by writing a 1 GiB file with dirty_ratio at 100; NFS has no delayed allocation and the file does not fit a 64 MiB export",
    "571": "_require_test_fcntl_setlease notruns on NFS whatever delegations are held: locktest -t does F_SETLEASE F_UNLCK on a file with no lease, generic_delete_lease() returns -EAGAIN when no lease matches (fs/locks.c), and common/rc turns exit code 11 into notrun for NFS only",
    "590": "an 8 GiB file and XFS's extent-size limit; the export is a 64 MiB tmpfs and NFS has no extents",
    "632": "src/detached_mounts_propagation creates detached mounts with open_tree() and checks their propagation across mount namespaces; the subject is VFS mount propagation, not the filesystem",
    "759": "fsx -h, hugepage-backed buffers: _require_thp needs transparent hugepages, and UML has none (mm/Kconfig's TRANSPARENT_HUGEPAGE depends on HAVE_ARCH_TRANSPARENT_HUGEPAGE, which arch/um does not select)",
    "760": "fsx -h with O_DIRECT: no transparent hugepages in UML, as generic/759",
    "772": "_require_file_attr notruns: NFS has .fileattr_get but no .fileattr_set, so vfs_fileattr_set() returns -ENOIOCTLCMD",
    "777": "_require_open_by_handle -N notruns: fs/nfs/export.c has no .fh_to_parent, so exportfs_can_encode_fh() refuses EXPORT_FH_CONNECTABLE and name_to_handle_at(AT_HANDLE_CONNECTABLE) returns EOPNOTSUPP",
    "780": "_require_file_attr notruns, as generic/772",
    "786": "_require_test_fcntl_setdeleg notruns: it probes a directory, NFS directories have no .setlease, and kernel_setlease() returns -EINVAL",
    "787": "_require_test_fcntl_setdeleg notruns, as generic/786 (the probe is on a directory)",
    "798": "cachestat()'s body is a static helper in mm/filemap.c reachable only through the syscall; the port would have to un-static it",
    "521": "soak test outside the auto group: fsx with O_DIRECT, 1000000 operations (about 4 minutes at the rate the generic/363 port runs on the VM). It could run through xfs_run_prog() like generic/091; not ported, for run time",
    "522": "soak test outside the auto group: fsx, 1000000 operations; see generic/521",
    "004": "O_TMPFILE: fs/nfs wires no .tmpfile inode operation, so the client cannot create one (upstream's _require_xfs_io_command \"-T\" notruns)",
    "402": "_require_timestamp_range notruns: _filesystem_timestamp_range() in common/rc has no nfs case, so the bounds are unknown",
    "452": "copies ls onto the mount and executes it, before and after a read-only remount. xfs_run_prog() can execute static binaries, from the host directory; running one from the NFS mount is not done yet",
    "685": "fzero (FALLOC_FL_ZERO_RANGE): nfs42_fallocate() accepts only mode 0 and PUNCH_HOLE with KEEP_SIZE; the suid/sgid rule itself is covered by the generic/683 and 684 ports",
    "686": "finsert (FALLOC_FL_INSERT_RANGE): not accepted by nfs42_fallocate(); see generic/685",
    "687": "fcollapse (FALLOC_FL_COLLAPSE_RANGE): not accepted by nfs42_fallocate(); see generic/685",
}

req_re = re.compile(r"^\s*(_require_[a-z0-9_]+|_fixed_by_[a-z0-9_]+)(.*)$", re.M)



LEAD = """# xfstests generic/* that are not ported, and why

[kunit/xfstests/](../kunit/xfstests/) holds %d ports of upstream's %d
`generic/*` cases. This file accounts for the other %d: every one of them
has a reason, and the reason names what specifically cannot be reproduced
rather than "it did not work".

Two rules decide most of it, both from
[kunit-nfs-reference.md](kunit-nfs-reference.md#the-xfstests-ports):

- A case upstream reports `[not run]` on an NFSv4.2 mount is not ported,
  because there is no upstream result to mirror. Most of the families
  below are that: the test asks for something NFS does not have.
- A case whose subject is a userspace program rather than the filesystem
  is not ported either.

The rest are limits of this fixture -- one kernel, one client, one server,
a tmpfs export, no block device -- and those say so. A few are possible
with what the fixture has but not ported yet, and those say that.

This file is generated by `scripts/kunit/xfstests-port-status.py`, which
holds the rules; rerun it after adding a port.

"""

req_re = re.compile(r"^\s*(_require_[a-z0-9_]+|_fixed_by_[a-z0-9_]+).*$", re.M)


def load(num):
    with open(os.path.join(TESTS, num), encoding="utf-8", errors="replace") as f:
        return f.read()


def groups_of(src):
    m = re.search(r"^_begin_fstest\s+(.*)$", src, re.M)
    return m.group(1).split() if m else []


def classify():
    """-> (ported, [(num, reason), ...])"""
    ported = sorted(p[:-2] for p in os.listdir(PORTED_DIR) if p.endswith(".c"))
    skipped = []
    for num in sorted(n for n in os.listdir(TESTS) if re.fullmatch(r"\d{3}", n)):
        if num in ported:
            continue
        src = load(num)
        reason = MANUAL.get(num)
        if not reason:
            for pat, why in SKIP_RULES:
                if re.search(pat, " ".join(m.group(0).strip()
                                           for m in req_re.finditer(src))):
                    reason = why
                    break
        if not reason:
            for g in groups_of(src):
                if g in SKIP_GROUPS:
                    reason = SKIP_GROUPS[g]
                    break
        if not reason:
            for pat, why in BODY_RULES:
                if re.search(pat, src, re.M):
                    reason = why
                    break
        if not reason:
            reason = "UNCLASSIFIED -- read this one and add it to MANUAL"
        skipped.append((num, reason))
    return ported, skipped


def wrap(nums, width=74):
    return textwrap.fill(" ".join("generic/%s" % n for n in nums), width=width)


def main():
    ported, skipped = classify()
    families = defaultdict(list)
    individual = []
    for num, reason in skipped:
        (individual if num in MANUAL else families[reason]).append(
            (num, reason) if num in MANUAL else num)

    with open(OUT, "w") as f:
        f.write(LEAD % (len(ported), len(ported) + len(skipped), len(skipped)))
        f.write("## Families\n\n")
        for reason, nums in sorted(families.items(), key=lambda kv: -len(kv[1])):
            f.write("### %s\n\n%d tests:\n\n```\n%s\n```\n\n" %
                    (reason[0].upper() + reason[1:], len(nums), wrap(sorted(nums))))
        f.write("## Examined individually\n\n| Test | Why not |\n|---|---|\n")
        for num, reason in sorted(individual):
            f.write("| generic/%s | %s |\n" % (num, reason))
        f.write("\n## Ported\n\n```\n%s\n```\n" % wrap(ported))
    print("%s: %d ported, %d not" % (OUT, len(ported), len(skipped)))


if __name__ == "__main__":
    main()
