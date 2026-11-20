# TODO

Open work, with a pointer to the evidence for each item. Updated 2026-11-20.

## Patches to send upstream

### 1. krb5i reply-page accounting

`patches/svc-send-account-reply-pages.patch`. Analysis in
`docs/krb5i-reply-page-reuse.md`. NFS reads over `sec=krb5i` fail with EIO
because the GSS integrity checksum lands on a page past `rq_next_page`,
which is reused while the socket still holds it.

Before sending:

- Replace the placeholder `Fixes:` with `5df5dd03a8f7` ("sunrpc: Use
  sendmsg(MSG_SPLICE_PAGES) rather then sendpage").
- Decide on `Cc: stable@vger.kernel.org` and `Assisted-by:`. Neither is there.
- In the doc's "Rejected alternatives", give the real reason the fix is not
  in `svcauth_gss_wrap_integ()`: the backchannel. `svc_process_bc()` replies
  from a buffer with `pages == NULL` (`net/sunrpc/svc.c:1723`), reaches
  `svc_authorise()`, and never reaches `svc_send()`. Also name
  `svc_rdma_save_io_pages()` (`net/sunrpc/xprtrdma/svc_rdma_sendto.c:1006`),
  which takes exactly `[rq_respages, rq_next_page)`.
- The doc justifies the conditional with `fs/nfsd/nfs3proc.c:598`, but that
  reservation is trimmed before `svc_send()` on success
  (`fs/nfsd/nfs3proc.c:628`). Cite a surviving error path instead: READLINK
  (`:193`) or READDIRPLUS `goto out` (`:670`).

### 2. krb5i WRITE not resent after a bad MIC (generic/761)

`patches/sunrpc-resend-after-rejected-reply.patch`; the mechanism is in the
patch description. Under krb5i, generic/761 fails with `io thread failed`
when the server rejects a WRITE with `AUTH_BADCRED` and the client re-decodes
the stored reply instead of resending. On nfsd-testing it failed 20 of 20
(CI run 44287876751). With the patch, 20 of 20 passed on the VM and in CI
run 44308912009.

Before sending:

- No `Fixes:` tag. Find the commit that made `rpc_task_need_encode()` skip
  already-sent `RPC_TASK_NO_RETRANS_TIMEOUT` tasks.
- Review the `call_status()` transport-error path, listed in the patch.
- Two `rpc_call_rpcerror tk_status=-5` per iteration on client `@00000002`,
  with and without the patch. Not identified.

### 3. NFSv3 SETACL rejects an empty access ACL

`patches/nfsd-acl-accept-empty-access-acl.patch`. Analysis and open points in
`docs/nfsd-v3-acl-setacl-regression.md`. Before sending: add
`Signed-off-by`. The NFS_ACLv2 half is untested.

### 4. xfstests: `_compare_range` leaves readers running (generic/118)

`patches/xfstests/0001-common-reflink-reap-_compare_range-readers.patch`,
applied in CI. Fixes the `test-118: Directory not empty` cleanup failure.
Before sending: add `Signed-off-by`.

## Issues found, no patch

### 5. read() returns EBUSY on NFS

`docs/nfs-read-ebusy.md`. `nfs_file_read()` propagates an
`invalidate_inode_pages2()` failure that `nfs_file_write()` ignores.
Triggered by `cce0be6eb497`. Precedent: `874f946376de`.

- Report to linux-nfs with the reproducer.
- Move the instrumentation from the VM (`~/gssdbg/ebusy-probes.diff`) into
  the repo.

### 6. nfs/001 under krb5i and krb5p

Setting a large NFSv4 ACL fails with `EINVAL`, per
`docs/krb5i-reply-page-reuse.md`. Not investigated. CI skips it
(`nfs4_setfacl utility required`): the `xfstests-uml` action does not install
`nfs4-acl-tools`.

## Known upstream, excluded

Reasons for both are in `scripts/xfstests-exclude`.

- generic/258, generic/634: timestamps lost under an NFSv4.2 delegation. Jeff
  Layton's backdated-timestamp series covers 258; 634's future dates stay
  clamped.
- generic/444: NFSv3 MKDIR cannot carry an ACL.

## CI and harness

### 7. generic/095 and item 5

In `-g quick` and not excluded, so any NFSv4.2 leg can flake on item 5.
Leave it, or exclude it with a link once item 5 is reported.

### 8. generic/761 on the coverage krb5i leg

`xfstests-uml.yml`'s krb5i leg carries item 2's patch. `coverage.yml`'s
`COVERAGE_PATCHES` does not, so its krb5i leg will usually fail 761. Options:
add the patch to `COVERAGE_PATCHES`, or add a krb5i exclude file. Excluding
also drops the read-back EIO case that item 1 fixes.

### 9. lcov `--ignore-errors negative` is unproven

`scripts/00-run-xfstests-in-uml.sh:251` and
`scripts/kunit/run-nfs-kunit.sh:372` pass it after `lcov` once aborted on a
negative count and left a 0-byte tracefile. Not recurred since. Confirm that a
leg hitting it still uploads a tracefile.

## Backports carried, not for upstream

Used only by the `kunit-v6-12-57-patched` job in `kunit.yml`.

- `patches/um-thread-info-in-task-v6.12.57.patch`: `2f681ba4b352`, for the
  host-signal livelock in `docs/kunit-nfs-reference.md`.
- `patches/overflow-kunit-define-flex-v6.12.57.patch`: `04e403e6627d`, fixes
  `DEFINE_FLEX_test` under GCC 15.
