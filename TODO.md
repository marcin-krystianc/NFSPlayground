# TODO

Patches to send, issues found, and the harness work each one leaves behind.
Every entry says where the evidence is. Updated 2026-10-08.

## Patches to send upstream

### 1. krb5i reply-page accounting

`patches/svc-send-account-reply-pages.patch`, "SUNRPC: account for Reply
pages the encode stream consumed". Analysis in
`docs/krb5i-reply-page-reuse.md`. NFS reads over `sec=krb5i` fail with EIO
because the RPCSEC_GSS integrity checksum is appended to a page past
`rq_next_page`, so the page is reused while the socket still references it.
Measured: `-g quick` krb5i from 16 failures to 2, reproducer from 2-3 of 20
failed reads to 0.

Before sending:

- The `Fixes:` line is still a placeholder,
  `<commit that introduced MSG_SPLICE_PAGES in svc_tcp_sendmsg>`. The commit
  is `5df5dd03a8f7` ("sunrpc: Use sendmsg(MSG_SPLICE_PAGES) rather then
  sendpage", David Howells, 2023-06-09), which added
  `.msg_flags = MSG_SPLICE_PAGES` to `svc_tcp_sendmsg()`. `e18e157bb5c8`
  later moved it into the `msghdr` initialiser.
- No `Cc: stable@vger.kernel.org` and no `Assisted-by: LLM`, unlike the
  krb5p patch below. Decide whether both belong on this one too.
- The posting will be asked why the fix is in `svc_send()` rather than at the
  end of `svcauth_gss_wrap_integ()`. Both run before `xpo_sendto()`, so the
  RDMA transport is not what separates them. The backchannel is:
  `svc_process_bc()` copies the client's `rq_snd_buf` into `rqstp->rq_res`
  (`net/sunrpc/svc.c:1723`), and that buffer has `buf->pages == NULL`
  (`xdr_buf_init()`, `include/linux/sunrpc/xdr.h:79`), so
  `rq_res_stream.page_ptr` there indexes nothing related to `rq_pages[]`. It
  reaches `svc_authorise()` like any request but never reaches `svc_send()`.
- `docs/krb5i-reply-page-reuse.md`'s "Rejected alternatives" paragraph should
  name that backchannel argument, and should name
  `svc_rdma_save_io_pages()` (`net/sunrpc/xprtrdma/svc_rdma_sendto.c:1006`),
  which moves exactly `[rq_respages, rq_next_page)` into the send ctxt and
  NULLs those slots. Both are stronger than the reasons currently given.
- The same doc justifies the conditional with NFSv3 page reservation
  (`fs/nfsd/nfs3proc.c:598`), but that reservation is trimmed before
  `svc_send()` on the success path (`fs/nfsd/nfs3proc.c:628`). The cases that
  actually survive are error paths: NFSv3 READLINK (`fs/nfsd/nfs3proc.c:193`),
  READDIRPLUS taking `goto out` past the trim (`fs/nfsd/nfs3proc.c:670`), and
  a failed `nfsd_iter_read()`. Cite one of those instead.

## Issues found, no patch

### 3. read() returns EBUSY on NFS

`docs/nfs-read-ebusy.md`. A buffered read gets `EBUSY` when
`invalidate_inode_pages2()` cannot release a folio, because `nfs_file_read()`
propagates the error while `nfs_file_write()` discards the identical one.
Triggered by `cce0be6eb497` (2025-12-31) making `nfs_release_folio()` fail
without waiting. Upstream called the same symptom a regression in
`874f946376de` (2015) and fixed it with a workaround that does not cover this
case. Reproduced locally with two `msleep()` calls widening existing windows.

Next steps:

- Report to linux-nfs with the reproducer, naming `cce0be6eb497` as the
  trigger and `874f946376de` as the precedent.
- The instrumentation is saved on the test VM at
  `~/gssdbg/ebusy-probes.diff` (94 lines: the probes plus the two `msleep`
  windows). It belongs in the repo so the reproduction is not lost.
- Decide what CI does meanwhile; see item 9.

### 4. generic/118 on the nconnect-4 leg

Seen once, CI run 37436016277, with coverage collected normally and one test
failed:

```
Failures: generic/118
rm: cannot remove '/mnt/nfs-test-env/test/test-118': Directory not empty
```

A cleanup failure at the end of a reflink compare. Not investigated. Unclear
whether it is the same class as the silly-rename `.nfsXXXX` problem or
something else.

### 5. nfs/001 under krb5i and krb5p

Fails to set a large NFSv4 ACL with `EINVAL` under both flavours, per
`docs/krb5i-reply-page-reuse.md`. CI never runs it: the log says
`nfs/001 [not run] nfs4_setfacl utility required`, because the
`xfstests-uml` action's apt list has no `nfs4-acl-tools`. So it is only
visible on a VM where that package is installed. Not investigated, and not
currently covered either.

### 6. generic/761 under krb5i

The test deliberately rewrites an O_DIRECT buffer during writeback, so the
integrity checksum computed over the *request* cannot match what the server
verifies. Inherent to the test under `sec=krb5i` rather than a kernel bug.
Not observed under krb5p.

It has two distinct failure modes under krb5i, and only one of them is item
1's bug. Comparing the two legs of CI run 37292153575, which differ only by
the fix:

- Unpatched (job 111704817318) failed on the `cat` that reads the file back:
  `cat: /mnt/nfs-test-env/scratch/foobar: Input/output error`, plus
  `_check_dmesg: something found in dmesg`. That is the reply-page reuse bug,
  and item 1's patch fixes it.
- Patched (job 112983217085, run 37670049296, 2026-10-07) failed in the
  O_DIRECT write loop instead: `io thread failed`, no dmesg complaint. That
  is `xfstests/src/dio-writeback-race.c:129-131`, where a `write()` of
  `blocksize` returned short or -1. The helper does not print errno, so the
  log cannot say which. This is the request-side mode, and nothing fixes it.

`docs/krb5i-reply-page-reuse.md:123-129` already recorded the second mode:
`-g quick` krb5i went from 16 failures to 2 with the patch, and 761 was one
of the two that remained.

The write mode is intermittent, as a deliberate data race run 256 times
would be. The patched krb5i leg passed 761 in runs 37436016204, 37480407624,
37608031266 and 37621082137, then failed it in 37670049296. Do not read
either outcome as a signal about item 1 or item 2. See item 13 for the
coverage decision this forces.

## Known upstream, not ours to fix

### 7. generic/258 and generic/634

Explicit timestamps lost under an NFSv4.2 timestamp delegation. Reasoning and
the trace evidence are in `scripts/xfstests-exclude`. Jeff Layton's series
"fs/nfsd: accept a backdated timestamp from a delegation holder" covers the
two past dates; per that series nfsd still clamps a future time, so
`generic/634` is expected to keep failing with delegations on. Both excluded.

### 8. generic/444

NFSv3 MKDIR cannot carry an ACL, so the client sends a separate SETACL and
the server clears S_ISGID for a non-member. A protocol limitation, reasoning
in `scripts/xfstests-exclude`. Excluded.

## CI and harness

### 9. Decide what to do about generic/095

It is in `-g quick` and in no exclude file, so every NFSv4.2 leg runs it and
any of them can flake on item 3. Either leave the legs red and track the
bug, or exclude it with a link to the upstream report once one exists.
`scripts/xfstests-exclude` allows "a genuine bug with a link to where it is
tracked", which now fits. This is a coverage decision.

### 10. generic/486 is still excluded for krb5p

`scripts/xfstests-exclude-krb5p` excludes it, and the comment still says
"These legs deliberately run the tree as released, so the patch is not
applied here. xfstests-uml.yml's krb5p-fix jobs run 486 with and without
it". Both statements are now false: the `krb5p` variant applies
`svcauth-gss-unwrap-priv-krb5p.patch`, and the `xfstests-uml-patch` job has
been removed. As it stands the leg applies the fix and then skips the only
test that exercises it. Dropping the entry would test the fix, and can also
turn the leg red if the fix is incomplete.

### 11. Dead v6.12.57 patch conditional

`.github/workflows/xfstests-uml.yml` still passes
`patches: ${{ matrix.linux_ref == 'v6.12.57' && 'um-thread-info-in-task-v6.12.57.patch' || '' }}`
while the matrix is `linux_ref: ['master']` only. Remove it, or restore
v6.12.57 to the matrix. The comment above the job also still explains the
v6.12.57 leg.

### 12. The lcov `negative` suppression is unexercised

`scripts/00-run-xfstests-in-uml.sh:251` and
`scripts/kunit/run-nfs-kunit.sh:506` now pass
`--ignore-errors mismatch,negative`, after `lcov` aborted on
`Unexpected negative count '-1' for mm/page-writeback.c:2979` and left a
0-byte tracefile. Run 37480407479 had all eight tracefiles at full size and
no `negative` warning in the leg that was checked, so the race did not recur
and the suppression has not been proven to work in CI. Confirm on a later run
that a leg which hits it logs `WARNING: ('negative')` and still uploads a
tracefile.

### 13. generic/761 will keep flipping the krb5i leg red

Item 6's write mode has no fix and no exclude entry. There is no krb5i
exclude file at all, so `-g quick` runs 761 on that leg every time and it
fails at random: once in the last five runs. Either create
`scripts/xfstests-exclude-krb5i`, wire it into the `krb5i` variant's
`exclude_file` in each workflow's matrix, and list 761 with item
6's mechanism as the reason, or leave the leg to flake and know why when it
does. The exclude rules want "a reason that is understood", and the
request-side checksum mechanism in `docs/krb5i-reply-page-reuse.md` is one.

Note that excluding it also drops coverage of the mode item 1's patch *does*
fix, the EIO on read-back. Nothing else in `-g quick` was seen to exercise
that.

## Backports carried, not for upstream

Both apply only to the `v6.12.57` leg, which the matrix no longer runs; see
item 11.

- `patches/nfs-eof-page-pollution-v6.12.57.patch`: backport of
  `b1817b18ff20`, `b2036bb65114` and `d5811e6297f3`, without which
  generic/363 fails. Not a straight cherry-pick; the header says what
  diverged.
- `patches/um-thread-info-in-task-v6.12.57.patch`: backport of
  `2f681ba4b352` ("um: move thread info into task"), for the host-signal
  livelock described in `docs/kunit-nfs-reference.md`. Also not a straight
  cherry-pick.
