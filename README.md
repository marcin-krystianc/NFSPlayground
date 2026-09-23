# NFSPlayground

A workbench for testing the Linux NFS client, and for understanding how VAST
NFS — a maintained out-of-tree fork of the kernel's NFS subtrees, shipped as
a source tarball — differs from the mainline code it replaces.

Two largely independent strands live here.

## 1. In-kernel tests for the NFS client

KUnit suites that run under User Mode Linux: no VM, no kernel install, no
separate server machine. Two kinds:

- **Unit suites** call NFS client and SunRPC functions directly — no mount,
  no network.
- **xfstests ports** run real filesystem tests against a real NFSv4.2 mount,
  served by knfsd inside the same UML kernel over loopback.

**This part is a proof of concept.** It demonstrates that both approaches
work; it is not coverage of the NFS client, and a green run is weak
evidence. [The numbers and the named gaps](docs/kunit-nfs.md#status) are in
the docs.

- **[docs/kunit-nfs.md](docs/kunit-nfs.md)** — start here: what it is, how
  to run it, what is and isn't covered.
- [docs/kunit-nfs-reference.md](docs/kunit-nfs-reference.md) — implementation
  notes: fixture mechanics, which seams make each area reachable, the
  constraints on what can be tested this way. Read before changing a test.
- [docs/xfstests-ports-not-done.md](docs/xfstests-ports-not-done.md) — the
  `generic/*` cases that are **not** ported, each with the reason.

```sh
scripts/fetch-sources.sh linux       # once
scripts/kunit/run-nfs-kunit.sh
```

Test sources are in `kunit/`; `scripts/kunit/run-nfs-kunit.sh` wires them
into a fetched kernel tree and drives `kunit.py`. `COVERAGE=1
scripts/kunit/run-nfs-kunit.sh` additionally produces an lcov/gcov HTML
report ([docs/kunit-nfs.md#coverage](docs/kunit-nfs.md#coverage)); the latest
`master` report is published at
[marcin-krystianc.github.io/NFSPlayground](https://marcin-krystianc.github.io/NFSPlayground/).

## 2. VAST NFS investigation

What the VAST fork actually changes, which of its features can be tested
without VAST hardware, and how it behaves under failure.

- [docs/vastnfs-vs-linux.md](docs/vastnfs-vs-linux.md) — VAST NFS 4.5.8
  compared to Linux v6.12.57, measured rather than estimated.
- [docs/testing-vast-features-without-hardware.md](docs/testing-vast-features-without-hardware.md)
  — which VAST-only features need a real cluster and which do not.
- [docs/vastnfs-multipath-failover.md](docs/vastnfs-multipath-failover.md) —
  what `remoteports=` multipath does when a node goes offline. Short version:
  it aggregates bandwidth, it is not high availability.
- [docs/xfstests-against-vastnfs.md](docs/xfstests-against-vastnfs.md) —
  running xfstests against vanilla NFS servers, then against the VAST
  modules.

## Also here

- [docs/xfstests-vs-pynfs.md](docs/xfstests-vs-pynfs.md) — what each suite
  actually tests, and where they differ.
- `patches/` — backports applied to the fetched kernel tree, currently the
  UML host-signal livelock fix needed for a full KUnit run on `v6.12.57`.
- `scripts/` — xfstests runners (GitHub CI, a VM plus Docker servers, a
  container) and `fetch-sources.sh`.

## Layout

`xfstests/`, `pynfs/` and `nfs-utils/` are git submodules. `linux/` and the
extracted `vastnfs-*/` tree are gitignored and fetched on demand by
`scripts/fetch-sources.sh`.
