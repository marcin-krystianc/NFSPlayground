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
scripts/kunit/run-nfs-kunit.sh
```

Extra arguments pass through to `kunit.py`, so a narrower run is:

```sh
scripts/kunit/run-nfs-kunit.sh "xfstests/generic/0*"
scripts/kunit/run-nfs-kunit.sh --raw_output
```

Build dependencies beyond a normal toolchain: `flex bison bc gawk libelf-dev libssl-dev`.

## Coverage

```sh
COVERAGE=1 scripts/kunit/run-nfs-kunit.sh
```

Turns on UML's gcov support and, after the run, writes `coverage/coverage.info`
and an HTML report to `coverage/html/index.html` via `lcov`/`genhtml`
(`apt install lcov`). This is the "Generating code coverage reports under
UML" path from
[running_tips.rst](https://docs.kernel.org/dev-tools/kunit/running_tips.html#generating-code-coverage-reports-under-uml):
UML is an ordinary process, so `CONFIG_GCOV` writes `.gcda` files straight
into the build dir, unlike the debugfs-based `CONFIG_GCOV_KERNEL` path in
[gcov.rst](https://docs.kernel.org/dev-tools/gcov.html) that other
architectures use.

The report covers every file the run touched, not just `kunit/`: the
unit suites reach into `fs/nfs`, `fs/nfs_common` and `net/sunrpc`, and the
xfstests ports additionally exercise `fs/nfsd`, `fs/namei.c` and the VFS
paths underneath the loopback mount.

Getting a real report out of this needed two workarounds for UML-specific
gcov/lcov bugs, both applied automatically by `run-nfs-kunit.sh` under
`COVERAGE=1`; see
[kunit-nfs-reference.md#coverage](kunit-nfs-reference.md#coverage) for the
root causes.

## CI

`.github/workflows/kunit.yml` runs:

- `kunit` — a matrix over `v6.12.57`, `v6.18.52`, `v7.2.6` and `master`,
  unpatched. The `v6.12.57` leg can hit the livelock above.
- `kunit-v6-12-57-patched` — `v6.12.57` with the UML fix applied, running
  the full suite to completion.
- `kunit-coverage` — `master` with `COVERAGE=1`, unfiltered. Uploads
  `coverage/coverage.info` and the HTML report as the `kunit-coverage-master`
  artifact, and puts the `lcov --summary` totals in the job's step summary.
  Runs on every push and PR, not just `master`.
- `publish-coverage-pages` — push-to-`master` only. Publishes
  `kunit-coverage`'s `coverage/` (the HTML report plus `coverage.info`) to
  GitHub Pages.
- `coverage-diff` — PR-only. Downloads the PR's own `coverage.info`
  (from `kunit-coverage` in the same run) and the latest successful
  push-to-`master` run's, via `actions/download-artifact`'s cross-run
  `run-id` support, then runs `scripts/kunit/coverage-diff.py` and
  posts/updates a single PR comment with the per-file delta. On a fork PR
  the default `GITHUB_TOKEN` is read-only, so the comment step no-ops
  instead of failing the job.

## Where the detail lives

[kunit-nfs-reference.md](kunit-nfs-reference.md) has the implementation
notes: how the fixtures are built, which seams make each area reachable,
what the runner does to the kernel tree, the livelock root cause and
backport, and the findings from individual ports. That file is the one to
read before changing a test; this one is the orientation.
