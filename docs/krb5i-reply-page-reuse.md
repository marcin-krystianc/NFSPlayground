# krb5i read failures: a knfsd reply-page accounting bug

NFS reads over a `sec=krb5i` mount fail intermittently with `EIO`: 16 of 641
tests in `xfstests -g quick`, a different subset each run. The cause is in
knfsd's reply-page bookkeeping. Fixed by
`patches/svc-send-account-reply-pages.patch`, whose commit message carries
the full reasoning and the measurements; this file is the orientation.

Code references are upstream master `4982d3552` (v7.3-rc3). Measured with
knfsd and the NFS client in one UML kernel over loopback, NFSv4.2,
`rsize=262144`, an MIT KDC in the guest, xfs-backed export
(`scripts/00-run-xfstests-in-uml.sh`, `NFS_SEC=krb5i`).

## The two bookkeepers

An nfsd thread owns one `svc_rqst` with a page array reused for every
request it handles. Two cursors index it:

- **`rq_next_page`** — one past the last reply page *in use*. Set up at
  `net/sunrpc/svc.c:1663`, maintained by whoever encodes.
- **`rq_res_stream.page_ptr`** — the XDR encode stream's own page cursor.
  Started from `rq_next_page - 1` for every request
  (`include/linux/sunrpc/svc.h:587`), so the two walk the same array.

The stream advances its cursor on its own: `xdr_get_next_encode_buffer()`
(`net/sunrpc/xdr.c`) does `xdr->page_ptr++` when a write fills the current
page. It does not touch `rq_next_page`. Callers must re-sync, and nfsd
does, once per operation (`fs/nfsd/nfs4xdr.c:6718`):

```c
	/* Account for pages consumed while encoding this operation.
	 * The xdr_stream primitives don't manage rq_next_page. */
	rqstp->rq_next_page = xdr->page_ptr + 1;
```

## Why `rq_next_page` must be right

Per request, pages cycle through three steps:

1. **Send.** `svc_send()` → `svc_tcp_sendmsg()` passes the reply to the
   socket with `MSG_SPLICE_PAGES`: the socket takes a *reference* and
   transmits later. `sendmsg` returns as soon as the data is queued.
2. **Release.** `svc_rqst_release_pages()` (`net/sunrpc/svc.c:968`) drops
   nfsd's reference on `[rq_respages, rq_next_page)` and **NULLs those
   slots**.
3. **Refill.** `svc_alloc_arg()` refills via `alloc_pages_bulk()`, which
   populates **only slots that are NULL on entry** (`mm/page_alloc.c:5150`).

That combination is what makes zero-copy safe: a page the socket still
references survives step 2 with a non-zero refcount, so the allocator
cannot hand it back in step 3, and nfsd gets a different page. The whole
guarantee rests on the page being inside `[rq_respages, rq_next_page)`.

## The bug

A 256 KiB READ reply fills its last data page exactly. nfsd encodes it and
syncs, so `rq_next_page` covers head plus 64 data pages. Then, *after* that
last sync, `svcauth_gss_wrap_integ()` appends the 40-byte integrity
checksum with `xdr_stream_encode_opaque()`. The stream has no room left, so
it advances `page_ptr` and writes the checksum into the **next** page —
one past `rq_next_page`. Nothing re-syncs: no auth flavour touches
`rq_next_page`.

That page is outside the released range, so step 2 neither drops the
reference nor NULLs the slot, and step 3 skips the non-NULL slot and keeps
the same page. The next reply from that thread writes *its* checksum there,
while the socket still holds the previous reply's reference. The earlier
reply is transmitted carrying the later reply's checksum; the client cannot
verify the MIC and returns `-EIO` from `gss_unwrap_resp_integ()`.

Most occurrences are masked by RPC retries, which is why only some surface
as `EIO` (one run: 11 bad MICs, 3 user-visible errors).

`sec=sys` is unaffected because nothing encodes after nfsd's last sync.

## The fix, and why in `svc_send()`

```c
	if (rqstp->rq_next_page < rqstp->rq_res_stream.page_ptr + 1)
		rqstp->rq_next_page = rqstp->rq_res_stream.page_ptr + 1;
```

- **It is the gate.** `svc_send()` (`net/sunrpc/svc_xprt.c:993`) is the only
  path by which a server reply reaches a transport, and it covers every
  transport and auth flavour at once. The backchannel bypasses it, correctly:
  `svc_process_bc()` replies out of the client `rpc_rqst`'s buffer, not this
  page array.
- **It is after every encoder.** `svc_authorise()`, which runs the GSS wrap,
  is called before it on the `sendit:` path.
- **It fits what the function already does.** Its first act reconciles the
  reply's byte accounting (`xb->len = head + page_len + tail`); this
  reconciles the page accounting.
- **A conditional, not an assignment**, because NFSv3 procedures reserve
  pages by advancing `rq_next_page` *ahead* of the stream
  (`fs/nfsd/nfs3proc.c:598`); assigning would un-reserve them. It is a
  no-op unless the stream really overran.
- **Dropped replies need no sync**: with no send there is no transport
  reference, so reusing the page is harmless.

Rejected alternatives: inside `svcauth_gss_wrap_integ()` (fixes one flavour,
and no auth code manages `rq_next_page`); inside `svc_authorise()` (also
runs on drop/close paths, and it releases auth state, not pages); inside the
XDR primitives (`xdr_stream` is shared with the client and has no
`svc_rqst`, which is why the convention exists).

A structural alternative worth noting: GSS already reserves byte-space for
the checksum (`svcxdr_set_auth_slack()`), and the privacy path keeps its
token in the tail kvec. Placing the integrity checksum in the tail too
would mean nothing ever spills. That is a rework of `wrap_integ`, not a
backportable fix.

## Evidence

| Claim | How it was checked |
|---|---|
| The error is MIC verification, not parsing | `rpcgss_verify_mic` trace event, `GSS_S_BAD_SIG` |
| Not a flaky computation | re-verified the same data and token twice more: identical result |
| The signed data is identical on both sides | hashed the MIC's byte range on server and client: same length, same hash |
| The client gets another reply's token | token sequence field named a reply the same nfsd thread signed 1–3 MICs later |
| Caused by zero-copy | dropping `MSG_SPLICE_PAGES` from `svc_tcp_sendmsg()`: 0 failures in 20 reads, 0 bad MICs |
| The checksum lands past `rq_next_page` | printed the stream cursor and the page array at the encode site |
| The fix works | reproducer 0/20 failures, 0 bad MICs (was 2–3/20 and 5–11) |
| No regression | `-g quick` krb5i: 16 failures → 2, no `EIO` in the run |

The two remaining failures are unrelated: generic/761 deliberately rewrites
an O_DIRECT buffer during writeback, so the checksum over the *request*
cannot match; nfs/001 fails to set a large NFSv4 ACL with `EINVAL` under
both krb5i and krb5p (and only runs where `nfs4-acl-tools` is installed, so
CI skips it).

## Reproducing

```sh
scripts/fetch-sources.sh linux                     # once
NFS_SEC=krb5i bash scripts/00-run-xfstests-in-uml.sh -g quick
```

Faster, without xfstests: in the guest, read a 4 MB file with
`dd bs=1M iflag=direct` in a loop, dropping caches between reads. 2 to 3 of
20 reads fail. Add `TRACE_EVENTS="rpcgss:rpcgss_verify_mic"` to see the
`GSS_S_BAD_SIG` events, including the ones retries hide.

In CI the patch is part of the `krb5i` variant in
`.github/xfstests-variants.json`, so the `xfstests-uml.yml` and
`coverage.yml` legs for that flavour run the suite with it applied. There is
no leg that runs krb5i without it, so the failure itself is not covered;
reproduce it by hand as above.
