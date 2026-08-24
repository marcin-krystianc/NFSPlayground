# xfstests vs. pynfs

What each suite in this repo actually tests, and where they differ. Based on
the checked-out trees at `xfstests/` (v2026.07.21) and `pynfs/` (pynfs-0.5).

## xfstests

Purpose: filesystem-behavior regression testing, from the client's point of
view. A test mounts a filesystem and drives it through the normal syscall
interface (`open`, `write`, `fsync`, `chmod`, ACLs, etc.), then checks the
observed behavior against a golden `.out` file.

NFS is one of many filesystems it supports, not the primary target. Support
is wired into `xfstests/common/rc`: `FSTYP=nfs` is handled explicitly for
mount/remount, version detection (`_nfs_version`), and consistency checks.

Two places tests live:

- `xfstests/tests/nfs/` — tests specific to NFS. Currently one:
  `001`, which checks that `nfs4_getfacl` doesn't return `ERANGE` when an ACL
  buffer size lands near a page boundary (a regression test for a specific
  fixed kernel bug, commit `ed92d8c137b7`).
- `xfstests/tests/generic/` — 1601 filesystem-agnostic tests, most of which
  can run against NFS via `FSTYP=nfs`. This is where the bulk of NFS coverage
  actually comes from: standard POSIX file operations, locking, xattrs,
  quotas, etc., run the same way they'd run against ext4 or XFS.

What it's good at: catching client-side regressions in ordinary file
workloads — the kind of thing a real application would notice. It exercises
the full path (VFS, client, network, server, backing store) as one system.

What it doesn't cover: protocol correctness in the RFC sense. It never sends
a malformed or edge-case NFSv4 COMPOUND directly; it only calls syscalls and
lets the client build whatever RPCs it builds.

## pynfs

Purpose: NFSv4 protocol-conformance testing, driven at the RPC level. Instead
of going through a mounted filesystem and the kernel client, pynfs builds and
sends NFSv4 COMPOUND operations directly against a server and checks the
returned status codes and results against RFC-mandated behavior.

Layout in the checked-out tree:

- `pynfs/nfs4.0/` — NFSv4.0 client and server test code, `servertests/`.
- `pynfs/nfs4.1/` — NFSv4.1 client/server code, with tests under
  `server41tests/st_*.py` (29 files: `st_open.py`, `st_lookup.py`,
  `st_delegation.py`, `st_sequence.py`, `st_xattr.py`, etc.), driven by
  `testserver.py`.
- `pynfs/rpc/`, `pynfs/xdr/` — the RPC/XDR layer used to construct raw
  NFSv4 operations by hand.

A sample from `st_open.py` (`testServerStateSeqid`, CODE `OPEN2`): it opens
the same file twice and asserts the server's `stateid.seqid` increments by
exactly one each time — a specific state-machine invariant from the spec,
not something a normal file-write workload would exercise or notice if
violated.

`pynfs/README` is explicit about scope: "Good for correctness testing,
handling of error/unlikely paths, any test where it is the order of RPCs
that matter. Not good for performance testing, tests where timing of RPC's
matter." It also warns that a failing test isn't automatically proof of a
server bug — results should be checked against the RFC before concluding
that.

## Comparison

| | xfstests | pynfs |
|---|---|---|
| Level | Syscall / VFS | Raw NFSv4 RPC (COMPOUND ops) |
| Drives | A real mounted filesystem | A hand-built protocol client, server optional |
| Scope in this repo | 1 NFS-specific test + 1601 generic tests (shared across all filesystems) | 29 server41tests files (NFSv4.1) + NFSv4.0 servertests |
| Good at | Regressions in real file workloads, cross-layer bugs | Protocol state-machine correctness, error paths, RPC ordering |
| Weak at | RFC-level protocol edge cases | Anything about real-world I/O performance or workload behavior |
| Failure meaning | Behavior changed vs. expected `.out` | Server response didn't match RFC-mandated behavior (needs manual RFC check per pynfs's own README) |

## Why both are referenced for this repo

`docs/vastnfs-vs-linux.md` names xfstests `-nfs` and pynfs together as the
tools for catching version-conditional breakage introduced by the VAST NFS
backport (Linux 6.12 NFS code built to run on kernels 4.15–7.0). They cover
different failure classes: xfstests would catch a backport breaking ordinary
file operations; pynfs would catch a backport breaking a specific NFSv4
state transition or error code that only shows up under protocol-level
testing, not under a normal workload.
