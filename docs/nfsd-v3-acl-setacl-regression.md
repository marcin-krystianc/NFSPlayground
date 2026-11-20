# NFSv3 SETACL regression in nfsd-testing

Since `8041c563f7f6` ("NFSD: Use xdrgen XDR functions for the NFS-ACL v3
SETACL procedure"), knfsd in `nfsd-testing` answers `ACL3ERR_INVAL` to a
SETACL request that the Linux NFSv3 client has sent for years. Over NFSv3,
`mkdir` in a directory with a default ACL then fails with EINVAL, although
the server has created the directory. xfstests generic/319 and generic/697
fail on the `v3` leg of `xfstests-uml.yml`. The NFSv4 legs pass them, since
NFSv4 does not use the NFS_ACL side protocol. The change is not in mainline.
The fix is `patches/nfsd-acl-accept-empty-access-acl.patch`.

## Symptom

CI run 44324335902, job "xfstests (UML, v3)", kernel `nfsd-testing`
`bc82014d1`: "Failures: generic/319 generic/697", "Failed 2 of 641 tests".

generic/319 sets a default ACL on a directory and creates a subdirectory:

```
+mkdir: cannot create directory '/mnt/nfs-test-env/scratch/testdir/testsubdir': Invalid argument
```

generic/697 fails at `vfstest.c:2009`, the `mkdirat()` in
`setgid_create_acl()`:

```
+vfstest.c: 2009: setgid_create_acl - Invalid argument - failure: create
```

A trace of nfs and nfsd events shows that the MKDIR succeeds and the
SETACL that follows it fails:

```
nfsd_vfs_create: xid=0x1de6fbff fh_hash=0x9a0584c1 type=DIR name=testsubdir
nfs_xdr_status: task:00000007@00000007 xid=0x1ee6fbff nfsaclv3 SETACL error=-22 (INVAL)
nfs_mkdir_exit: error=-22 (INVAL) name=00:21:131/testsubdir
```

## What the client sends

`nfs3_proc_mkdir()` (`fs/nfs/nfs3proc.c`) calls `posix_acl_create()`,
then `nfs3_proc_setacls()` on the new directory.

`posix_acl_create()` (`fs/posix_acl.c`) clones the parent's default ACL
into an access ACL for the new inode. When `posix_acl_create_masq()`
returns 0, the access ACL is equivalent to the mode, and it is released:
`*acl` stays NULL. For a directory, `*default_acl` is still the parent's
default ACL.

`__nfs3_proc_setacls()` (`fs/nfs/nfs3acl.c`) skips the call only when
there is nothing to set:

```c
if (acl == NULL && (!S_ISDIR(inode->i_mode) || dfacl == NULL))
	goto out;
```

For a directory with a default ACL it sends a SETACL with
`.mask = NFS_ACL` (from `b7fa0554cf1ba`, 2005) plus `NFS_DFACL`.
`nfsacl_encode()` writes an entry count of 0 for the NULL access ACL. The
skip condition is from `f87d928f6d986` (2014).

generic/319's default ACL `u::rwx,g::rwx,o::-` always produces an access
ACL equivalent to the mode. So does generic/697's
`u::rwx,g::rw,o::rwx,m:rw`, for its `mkdirat()`.

## What the server did before

Up to `414c1ecd3876`, `nfs3svc_decode_setaclargs()` decoded each list with
`nfs_stream_decode_acl()` (`fs/nfs_common/nfsacl.c`). With zero entries,
`xdr_nfsace_decode()` is never called, no ACL is allocated, and
`*pacl` is set to NULL. `nfsd3_proc_setacl()` then calls

```c
set_posix_acl(&nop_mnt_idmap, fh->fh_dentry, ACL_TYPE_ACCESS, argp->acl_access);
```

with NULL, which removes the access ACL. The request succeeds.
`nfsaclsvc_decode_setaclargs()` handled NFS_ACLv2 the same way.

## What changed

Chuck Lever's NFS_ACL xdrgen series, authored 2026-10-25, replaces those
decoders.

1. `da5bc1fc901f` ("NFSD: Implement NFS-ACL v3 ACL encoding and decoding
   with xdrgen") adds `nfs_acl3_secattr_decode_begin()` in
   `fs/nfsd/nfs3acl.c`. It rejects an empty access list:

   ```c
   if (!dfacl && !c->count) {
           argp->invalid = true;
           return true;
   }
   /* An empty default list publishes NULL: default ACL removal. */
   ```

   `nfsd3_proc_setacl()` turns `argp->invalid` into `ACL3ERR_INVAL`. At
   this commit SETACL still uses `nfs3svc_decode_setaclargs()`, so the
   check is not reached.
2. `8041c563f7f6` ("NFSD: Use xdrgen XDR functions for the NFS-ACL v3
   SETACL procedure") sets `[ACLPROC3_SETACL].pc_decode` to
   `nfs_acl_svc_decode_SETACL3args`. From here the check runs, and the
   client's SETACL fails.

The commit message of `da5bc1fc901f` says the hooks "hold the posix_acl
logic factored from nfs_stream_encode_acl() and nfs_stream_decode_acl()".
It does not mention a behaviour change for an empty access list.

`321d5bf51504` ("NFSD: Implement NFS-ACL v2 ACL encoding and decoding with
xdrgen") adds the same check to `nfs_acl2_secattr_decode_begin()` in
`fs/nfsd/nfs2acl.c`. The Linux client has no NFS_ACLv2 SETACL (no
`ACLPROC2_SETACL` in `fs/nfs`), so these tests cannot reach it.

## When

`nfsd-testing` is rebased, so the change has had several SHAs. Its
committer date moves with each rebase.

| nfsd-testing head | SETACL commit | Committer date |
|---|---|---|
| `6ab6af61dba3` | `3d8bbbc05418` | 2026-11-09 |
| `ecb3bfd5595f` | `22c050b61ece` | 2026-11-12 |
| `8489270d1cb9` (current) | `8041c563f7f6` | 2026-11-16 |

`git range-diff` shows `22c050b61ece` and `8041c563f7f6` are identical.
Every head above contains the zero-count check. Mainline
(`4405c18c95d2`) does not contain `8041c563f7f6`.

This repository's CI builds `nfsd-testing` by default since `769ac7b`
("Use nfsd-testing (#18)", 2026-11-13). The 2026-11-10 run already built
`nfsd-testing`, from git.kernel.org. All full `v3` runs checked fail
generic/697: 2026-11-10, 11-13, 11-16 (two), 11-17 and 11-18 (three).

## How the commit was isolated

All runs are NFSv3, sec=sys, generic/319 and generic/697 only, in UML on
the VM (`~/repro-acl`), unless marked CI.

| Kernel | Change to the tree | Result |
|---|---|---|
| `414c1ecd3876` | none | Passed all 2 |
| `414c1ecd3876` (CI job `v3-acl-414c1ecd3876`) | none | Passed all 2 |
| `91f2acdea432` (parent of `da5bc1fc901f`) | build fix | Passed all 2 |
| `da5bc1fc901f` | build fix | Passed all 2 |
| `b29aef0e2fca` (parent of `8041c563f7f6`) | build fix | Passed all 2 |
| `8041c563f7f6` | build fix | Failed 2 of 2 |
| `8041c563f7f6` | build fix, check removed | Passed all 2 |
| `83b0ca4b18ab` | none | Failed 2 of 2 |
| `83b0ca4b18ab` (CI job `v3-acl-83b0ca4b18ab`) | none | Failed 2 of 2 |
| `00d5ed4bfe87` (in the `6ab6af61dba3` history) | none | Passed all 2 |
| `6ab6af61dba3` | none | Failed 2 of 2 |
| `bc82014d199f` | check removed | Passed all 2 |
| `8489270d1cb9` | none | Failed 2 of 2 |

Every failure shows `nfsaclv3 SETACL error=-22 (INVAL)` in the trace. The
`bc82014d199f` run also carried an unrelated client change to
`net/sunrpc/clnt.c`; the unpatched run it was compared with had the same
change.

### Commits that do not build

The 45 commits after `414c1ecd3876` and before `83b0ca4b18ab` do not
build:

```
fs/nfsd/nfs4proc.c:1165:13: error: implicit declaration of function 'nfsd_read_io_mode'
```

`6b10cf47fb2c` ("NFSD: add tracing for how direct-mode READ and WRITE are
serviced", Mike Snitzer) adds the call in `nfsd4_read()`.
`83b0ca4b18ab` ("NFSD: Apply per-export I/O modes to READ and WRITE",
Jeff Layton) defines the function. Both have committer date
2026-11-16 13:35:07 -0500. The NFS_ACL series lies inside that range.

So without changes to the tree, the narrowest testable range is
`414c1ecd3876` (good) to `83b0ca4b18ab` (bad), which the two CI jobs in
`xfstests-uml.yml` run. The per-commit results above used a build fix
that replaced the call with the global `nfsd_io_cache_read`. Before
`83b0ca4b18ab` there are no per-export modes, and `nfsd_read_io_mode()`
returns that global for an export with the default mode. That fix is
not kept in this repository.

## Fix

`patches/nfsd-acl-accept-empty-access-acl.patch` applies to
`nfsd-testing` `8489270d1cb9`. It removes the zero-count check from
`nfs_acl3_secattr_decode_begin()` and `nfs_acl2_secattr_decode_begin()`.
An empty access list then publishes NULL, as an empty default list
already does. The SETACL procedures pass NULL to `set_posix_acl()`,
which removes the access ACL, as before the series.
`posix_acl_from_nfsacl()` returns 0 for a NULL ACL, so the decode-end
hooks accept it.

Kept as they are: the count-mismatch check, the
`NFS_ACL_MAX_ENTRIES` limit, and the per-entry type, permission and id
checks.

### Testing

On the VM, `nfsd-testing` `8489270d1cb9`, NFSv3, sec=sys,
`xfstests -g acl`, the same 15 tests each time:

| Kernel | Result |
|---|---|
| `8489270d1cb9` | Failed 2 of 15: generic/319, generic/697 |
| `8489270d1cb9` with the patch | Passed all 15 |

Six of the 15 report "not run" in both: generic/026, 077, 318, 389, 449
and nfs/001. The trace has two `nfsaclv3 SETACL error=-22` lines without
the patch and none with it. The patch also applies with `git apply` to
`83b0ca4b18ab`, the bad end of the CI range. It has not been run there.

## Open points

- The 2026-11-10 CI run (job 129882589739, kernel `6ab6af61dba3`) passed
  generic/319 and failed generic/697. On the VM the same kernel fails
  both. The xfstests submodule was the same (`56c410ad`). The cause is
  not known.
- The NFS_ACLv2 half of the fix is not exercised by any test here.
- The patch has no `Signed-off-by`. Add one before sending it.
