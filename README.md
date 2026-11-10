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
`master` report, merged with UML xfstests coverage by
[.github/workflows/coverage.yml](.github/workflows/coverage.yml), is published at
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

## Fixes these tests found

knfsd bugs first seen here as xfstests failures, and where the fix landed.
Both fixes are in Chuck Lever's `nfsd-testing` branch and have not reached
mainline yet, so `coverage.yml` still applies both to the tree it builds.

- **RPCSEC_GSS privacy left the decode stream stale.** After `gss_unwrap()`
  shortens the head iovec by the GSS token header and the confounder,
  `svcauth_gss_unwrap_priv()` adjusted only `xdr->nwords`, leaving
  `xdr->end` 32 bytes past the plaintext. An argument spanning the head and
  the page array was then consumed out of step, and nfsd decoded the next
  COMPOUND operation from argument data and returned `NFS4ERR_OP_ILLEGAL`.
  Found with `generic/486` under `sec=krb5p`, which failed in that
  configuration and nowhere else.
  Fix by us: [`97bf905e837e`](https://git.kernel.org/pub/scm/linux/kernel/git/cel/linux.git/commit/?id=97bf905e837e345107c2ed3efb2e65108e081e0e)
  "SUNRPC: reset the svc decode stream after unwrapping a privacy request"
  ([posting](https://patch.msgid.link/20261007204917.1818086-1-marcin.krystianc@gmail.com)).
  `Fixes: 42140718ea26`, so the bug dates from v6.3.

- **The GSS integrity checksum was written past `rq_next_page`.** The MIC is
  encoded after the last operation, onto a page beyond `rq_next_page`, so
  `svc_rqst_release_pages()` never releases it and the next reply from the
  same nfsd thread overwrites it while TCP still holds a `MSG_SPLICE_PAGES`
  reference to the previous one. The client receives another reply's token.
  Found here as NFS reads over `sec=krb5i` failing with `EIO`; the analysis
  and the evidence are in
  [docs/krb5i-reply-page-reuse.md](docs/krb5i-reply-page-reuse.md).
  Fix by Abhinandan Ekande:
  [`10cbdb3691a6`](https://git.kernel.org/pub/scm/linux/kernel/git/cel/linux.git/commit/?id=10cbdb3691a67c97ff8761965caa0e46a911094d)
  "SUNRPC: in svcauth_gss_wrap_integ() resync rq_next_page after the GSS
  wrap"
  ([posting](https://patch.msgid.link/20260922134858.2219136-1-aekande@redhat.com)).
  Found independently of this workbench: that commit credits mmap and
  fscache workloads, carries no `Reported-by`, and blames
  `d7de37d6d7cc` where the analysis here blamed `5df5dd03a8f7`. It resyncs
  in `svcauth_gss_wrap_integ()` rather than in `svc_send()`, which is where
  `patches/svc-send-account-reply-pages.patch` does it.

Bugs found here that are still unfixed are tracked in [TODO.md](TODO.md).

## Also here

- [TODO.md](TODO.md) — the patches waiting to be sent, the bugs found so far,
  and the harness decisions each one leaves open.
- [docs/krb5i-reply-page-reuse.md](docs/krb5i-reply-page-reuse.md) — NFS reads
  over `sec=krb5i` failing with `EIO`, a knfsd reply-page accounting bug.
  Patch in `patches/`.
- [docs/nfs-read-ebusy.md](docs/nfs-read-ebusy.md) — a buffered `read()` on
  NFS returning `EBUSY`, which is why generic/095 flakes. Client bug, no
  patch yet.
- [docs/xfstests-vs-pynfs.md](docs/xfstests-vs-pynfs.md) — what each suite
  actually tests, and where they differ.
- `patches/` — two kinds. Backports for the `v6.12.57` KUnit job: the UML
  host-signal livelock fix needed for a full run, and the NFS client's 'eof
  page pollution' fix, without which generic/363 fails. Plus the two knfsd
  fixes above, which `coverage.yml` applies through `COVERAGE_PATCHES`
  because it builds mainline, where neither has landed. `xfstests-uml.yml`
  builds `nfsd-testing` and needs no patches.
- `scripts/` — xfstests runners (GitHub CI, a VM plus Docker servers, a
  container) and `fetch-sources.sh`.

## Layout

`xfstests/`, `pynfs/` and `nfs-utils/` are git submodules. `linux/` and the
extracted `vastnfs-*/` tree are gitignored and fetched on demand by
`scripts/fetch-sources.sh`.

