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
    (r"_require_xfs_io_command\s+\"?(falloc -k|fpunch|fzero|fcollapse|finsert|funshare|zero)",
     "fallocate mode unsupported over NFSv4.2 (only ALLOCATE/DEALLOCATE exist)"),
    (r"_require_xfs_io_command\s+\"?(chattr|label|scrub|repair|bulkstat|fsmap|inject|resblks|parent|utimes|syncfs|lsattr)",
     "xfs_io command with no NFS/VFS equivalent"),
    (r"_require_aio|_require_aiodio", "libaio: no in-kernel equivalent"),
    (r"_require_fio", "fio: userspace workload generator"),
    (r"_require_freeze", "filesystem freeze: not an NFS operation"),
    (r"_require_attrs\s+trusted|_require_attr_v1", "trusted xattr namespace: not carried over NFS"),
    (r"_require_scratch_size_nocheck|_require_scratch_size|_scratch_mkfs_sized",
     "needs a filesystem of a chosen size (mkfs): no NFS equivalent"),
    (r"_require_loop", "loop device"),
    (r"_require_block_device|_require_local_device", "needs a real block device"),
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
    "aio": "libaio/io_submit: no in-kernel equivalent",
    "acl": "POSIX ACLs are NFSv3-only in the Linux client; the fixture mounts v4.2",
    "atime": "atime mount options have no effect on NFS: upstream _require_atime notruns",
    "dax": "DAX",
    "dedupe": "reflink/dedupe: not an NFS operation",
    "fiexchange": "FIEXCHANGE_RANGE (exchangerange) ioctl: not an NFS operation",
    "swapext": "the XFS swapext ioctl: not an NFS operation",
    "io_uring": "io_uring: no in-kernel equivalent",
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
     "driven by a userspace random-operation generator (fsstress/fsx): the "
     "port would be a reimplementation of the generator, and 011/013 "
     "(dirstress) and 075 (fsx) already cover that shape"),
    (r"src/t_stripealign|_scratch_resvblks|_xfs_force_bdev", "XFS geometry tooling"),
    (r"_scratch_dev_pool|_require_scratch_dev_pool", "needs a pool of block devices"),
    (r"_require_scratch_delalloc",
     "upstream's _require_scratch_delalloc notruns: it needs filefrag to report "
     "a delayed-allocation extent, and NFS has neither"),
    (r"_require_io_uring", "io_uring: no in-kernel equivalent"),
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
    "128": "needs to exec a setuid binary from the mount with nosuid set; a KUnit case cannot exec userspace, and the fixture mounts once",
    "241": "dbench, a userspace workload generator",
    "339": "src/dirhash_collide generates names that collide in the XFS and btrfs directory hashes; the fixture's server is tmpfs, which has no such hash",
    "345": "holetest -F: generic/340 and 344 with processes instead of threads. In a kernel test both markers are kthreads sharing one mm, so the port would duplicate generic/340",
    "436": "seek_sanity_test cases 13-16 are gated on unwritten extents -- space fallocate has reserved that SEEK_HOLE still reports as a hole. ALLOCATE against the tmpfs export allocates real zeroed pages, so the program's own probe turns these cases off",
    "445": "seek_sanity_test case 17, gated on unwritten extents for the same reason as generic/436",
    "460": "the bug is XFS's delalloc indirect-block reservation, reached by writing a 1 GiB file with dirty_ratio at 100; NFS has no delayed allocation and the file does not fit a 64 MiB export",
    "478": "OFD locks across clone(2), dup(2) and close: the subject is file-descriptor ownership across processes and fd tables, which a KUnit case does not have",
    "504": "the assertion is the contents of /proc/locks, which needs procfs mounted in the test kernel and a lock whose owning process has exited",
    "524": "the race is between XFS writeback's cached extent mapping and a truncate; NFS has no block mapping to cache",
    "571": "the lease test is src/locktest's two-process client/server protocol; nfs4_setlease() exists, but what the test drives is the harness",
    "590": "an 8 GiB file and XFS's extent-size limit; the export is a 64 MiB tmpfs and NFS has no extents",
    "597": "toggles fs.protected_symlinks and fs.protected_hardlinks, which are static ints in fs/namei.c with no in-kernel setter; what they gate is enforced by the client's VFS (may_follow_link/may_linkat), not by NFS",
    "598": "toggles fs.protected_regular and fs.protected_fifos, static ints in fs/namei.c; see generic/597",
    "604": "mounts and unmounts the filesystem under test to race umount against mount; the fixture's single deployment is shared by every suite in the run",
    "632": "detached mounts and mount-namespace propagation",
    "754": "the attributes are set in the trusted namespace (attr -R) on symlinks, which NFSv4.2 does not carry; what remains is symlink-target length coverage, which generic/309 and generic/360 already provide",
    "759": "fsx on hugepage-backed userspace buffers",
    "760": "fsx with O_DIRECT on hugepage-backed userspace buffers",
    "761": "the property is that a filesystem which checksums data falls back to buffered writes when the source buffer changes mid-write; NFS does not checksum data",
    "772": "file_getattr()/file_setattr() work on fsxattr -- project id, extent-size hints -- which NFS does not have",
    "777": "the property is decoding a connectable file handle after a mount cycle; the fixture's single deployment is shared by every suite, so the cycle is not available (handle encode/decode itself would be portable: the client does implement export_operations, fs/nfs/export.c)",
    "780": "file_getattr()/file_setattr() on special files; see generic/772",
    "786": "directory delegations via the F_SETDELEG fcntl, which does not exist on v6.12.57 -- one of the kernels this repo's CI builds against -- and src/locktest's two-process harness",
    "787": "file delegations via F_SETDELEG; see generic/786",
    "798": "cachestat()'s body is a static helper in mm/filemap.c reachable only through the syscall; the port would have to un-static it",
    "565": "copy_file_range between two filesystems; the fixture has one mount",
    "591": "src/splice-test's concurrent reader and writer are two processes sharing a pipe across a fork; the splice side itself is covered by the generic/249 port",
    "004": "O_TMPFILE: fs/nfs wires no .tmpfile inode operation, so the client cannot create one (upstream's _require_xfs_io_command \"-T\" notruns)",
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
a tmpfs export, no block device, no second process -- and those say so.

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
