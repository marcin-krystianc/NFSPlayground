# KUnit tests for NFS

In-kernel tests for the Linux NFS client, run under User Mode Linux with
`kunit.py`. No VM, no kernel install, no separate server machine: UML
builds an `ARCH=um` kernel and runs it as an ordinary process.

Two kinds of test live here:

- **Unit suites** (`kunit/*.c`) — call NFS client and SunRPC functions
  directly and assert on what they return. No mount, no network.
- **xfstests ports** (`kunit/xfstests/`) — real filesystem tests run
  against a real NFSv4.2 mount, served by knfsd inside the same UML
  kernel, over loopback.

## Status

**This is a proof of concept, not a test suite anyone should rely on yet.**
It shows that both approaches work and are worth continuing. It does not
show that the NFS client is well tested.

| | Covered | Out of |
|---|---|---|
| xfstests `generic/*` cases ported | 43 | 798 upstream |
| `net/sunrpc` files with unit tests | 3 | 27 |
| `fs/nfs` files with unit tests | 5 | 56 |

And within the files that *are* covered, coverage is partial by
construction. `nfs4proc.c` is about 11,000 lines, almost all of it issuing
RPCs; the unit tests reach a few dozen pure decision functions underneath
that, not the RPC paths themselves.

What this means in practice: a green run is evidence that the specific
behaviours pinned here still hold. It is not evidence that a change to the
NFS client is safe. Treat a failure as informative and a pass as weak.

Known gaps worth naming:

- **Crash consistency is not covered at all.** The fixture runs client and
  server in one kernel over tmpfs, so there is no way to drop writes and
  replay.
- **Nothing is exercised end to end over anything but loopback TCP
  NFSv4.2.** No pNFS, no RDMA, no `RPCSEC_GSS`/Kerberos mount. The pNFS
  and Kerberos coverage that exists is pure logic: layout-range arithmetic
  here, plus upstream's own `gss_krb5_test.c`, which the runner enables via
  `CONFIG_KUNIT_ALL_TESTS=y` but which is not this repo's work.
- The ported xfstests cases are chosen for being reachable, not for being
  the most valuable 43 of the 798.

## Running

```sh
scripts/fetch-sources.sh linux    # once: ~1.6 GB
scripts/kunit/run-sunrpc-kunit.sh
```

Extra arguments pass through to `kunit.py`, so a narrower run is:

```sh
scripts/kunit/run-sunrpc-kunit.sh "xfstests/generic/0*"
scripts/kunit/run-sunrpc-kunit.sh --raw_output
```

Build dependencies beyond a normal toolchain: `flex bison bc gawk libelf-dev libssl-dev`.

## Three things that will confuse you

**Spurious failures with UBSAN on.** The stock config enables UBSAN, which
reports a misaligned access in `kernel/exit.c` that has nothing to do with
NFS. KUnit blames whichever test happened to be running, so the "failure"
moves between runs. Add `--kconfig_add CONFIG_UBSAN=n` for a clean run, or
check the raw output for an `EXPECTATION FAILED` line — a UBSAN stack trace
without one is the artefact, not a real failure.

**A full run on `v6.12.57` can hang forever.** That is an upstream UML bug:
a race in the host-signal handling that predates anything here, fixed
upstream by commit `2f681ba4b352` and never backported to the 6.12 stable
series. `patches/um-thread-info-in-task-v6.12.57.patch` backports it, and
CI runs one job with the patch applied and one without. Later kernels in
the CI matrix (`v6.18.52`, `v7.2.6`, `master`) are unaffected.

**A green result says nothing about the kernel log.** Nothing fails a suite
for emitting WARNs, and the default `kunit.py` output does not show kernel
log lines at all.

## CI

`.github/workflows/kunit.yml` runs two jobs:

- `kunit` — a matrix over `v6.12.57`, `v6.18.52`, `v7.2.6` and `master`,
  unpatched. The `v6.12.57` leg can hit the livelock above.
- `kunit-v6-12-57-patched` — `v6.12.57` with the UML fix applied, running
  the full suite to completion.

## Where the detail lives

[kunit-nfs-reference.md](kunit-nfs-reference.md) has the implementation
notes: how the fixtures are built, which seams make each area reachable,
what the runner does to the kernel tree, the livelock root cause and
backport, and the findings from individual ports. That file is the one to
read before changing a test; this one is the orientation.
