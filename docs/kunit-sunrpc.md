# KUnit tests for SunRPC

Unit tests for pure-logic parts of SunRPC, run under User Mode Linux with
`kunit.py`. No VM and no kernel install involved; a full run takes a few
seconds.

## Running

```sh
LINUX_FULL=1 scripts/fetch-sources.sh linux    # once: full tree, ~1.6 GB
scripts/kunit/run-sunrpc-kunit.sh
```

`kunit.py` needs a complete kernel tree, which the default sparse checkout
(10 paths, enough for diffing against the VAST bundle) is not.
`LINUX_FULL=1` makes `fetch_linux()` run `git sparse-checkout disable`
instead of narrowing, fetching the remaining blobs for the pinned
`v6.12.57`.

Extra arguments pass through to `kunit.py`, e.g. `--raw_output`.

Build deps beyond a normal toolchain: `flex bison bc gawk libelf-dev
libssl-dev`.

**Expect two reported failures.** They are not test failures: UBSAN emits
an unrelated report that KUnit attributes to whichever test is running.
See [The UBSAN artefact](#the-ubsan-artefact-and-a-correction) before
chasing one. Adding `--kconfig_add CONFIG_UBSAN=n` gives a clean run, at
the cost of UBSAN coverage.

## What is tested

All four files under test are pure logic with no I/O, allocation or
locking, which is what makes them suitable for unit testing where most of
`fs/nfs` is not (see [xfstests-vs-pynfs.md](xfstests-vs-pynfs.md)), and
none had any tests before this.

`net/sunrpc/addr.c` is 354 lines of string ↔ `sockaddr` conversion with
four exported entry points. `rpc_pton()` is the primitive every
address-bearing NFS mount option is parsed through, including each address
in VAST's `remoteports=`/`localports=` lists.

`net/sunrpc/timer.c` is 123 lines implementing the RPC round-trip time and
variance estimator, three exported functions of integer arithmetic. It
decides retransmission timeouts for RPC over datagram transports.

`fs/nfs_common/common.c` is 201 lines of NFS status to errno translation,
three exported table lookups. They decide which errno an application
ultimately sees for a given server response, so a wrong entry is a silent,
protocol-visible bug.

`net/sunrpc/xdr.c` and the inline helpers in `include/linux/sunrpc/xdr.h`
implement the wire format every NFS operation travels over. XDR's defining
rule is that objects are padded out to a 4-byte boundary and the padding is
zero-filled (RFC 4506), which is where its classic bugs live.

Forty-seven suites, 438 cases, across seven files.

`kunit/addr_test.c` covers `net/sunrpc/addr.c`:

| Suite | Covers |
|---|---|
| `sunrpc-addr-pton` | parsing valid IPv4/IPv6, rejecting malformed input, the `salen` and `INET_ADDRSTRLEN` guards, IPv6 scope ids |
| `sunrpc-addr-ntop` | formatting, unsupported address families, scope-id suffix rules |
| `sunrpc-addr-roundtrip` | `sockaddr → string → sockaddr` and `sockaddr → uaddr → sockaddr` preserve address and port |
| `sunrpc-addr-uaddr` | RFC 5665 `h.h.h.h.p1.p2` form, both directions, plus malformed input |

`kunit/nfs_common_test.c` covers `fs/nfs_common/common.c`, the NFS status
to errno translation shared by client and server:

| Suite | Covers |
|---|---|
| `nfs-errno-v23` | `nfs_stat_to_errno()` table entries, the `-EIO` default, and that `NFSERR_EAGAIN` is deliberately *not* mapped |
| `nfs-errno-v4` | `nfs4_stat_to_errno()` for both tables, plus all four edges of the `10000 < stat <= 10100` pass-through window |
| `nfs-errno-localio` | `nfs_localio_errno_to_nfs4_stat()` reverse mappings, the `NFS4ERR_SERVERFAULT` default, and table precedence |
| `nfs-errno-roundtrip` | status round trips, and the deliberate `NFS4ERR_SERVERFAULT` asymmetry |

`kunit/xdr_test.c` covers the XDR codec:

| Suite | Covers |
|---|---|
| `sunrpc-xdr-align` | `xdr_align_size()`, `xdr_pad_size()`, `XDR_QUADLEN()`, and that object plus pad always fills whole XDR units |
| `sunrpc-xdr-primitives` | opaque/string/netobj encode and decode, zero-filled padding, length-prefix layout, and the `XDR_MAX_NETOBJ` and caller-limit rejections |
| `sunrpc-xdr-stream` | `xdr_stream` round trips for u32, u64, bool, fixed and variable opaques, uint32 arrays, and present/absent discriminators |
| `sunrpc-xdr-overflow` | decoding past the end returns `-EBADMSG`, `xdr_inline_decode()` returns NULL, and encoding past capacity returns `-EMSGSIZE` |
| `sunrpc-xdr-subsegment` | `xdr_buf_subsegment()` window arithmetic including both bounds edges, and `xdr_buf_trim()` clamping at empty |

`kunit/nfs4session_test.c` covers the NFSv4.1 session slot table in
`fs/nfs/nfs4session.c`:

| Suite | Covers |
|---|---|
| `nfs4-slot-alloc` | slots issued lowest-first without repeats, `-EBUSY` on exhaustion, freed slots reused |
| `nfs4-slot-accounting` | `highest_used_slotid` recomputed rather than decremented when the top slot is freed, `NFS4_NO_SLOT` when idle, `-E2BIG` lookups, target slotid updates |

This file needs `CONFIG_NFS_V4=y` and `CONFIG_NFS_V4_1=y`, neither of which
is in the stock `.kunitconfig`; the runner adds both.

`kunit/inode_test.c` covers attribute-freshness comparison in
`fs/nfs/inode.c`:

| Suite | Covers |
|---|---|
| `nfs-inode-attr-cmp` | `nfs_inode_attrs_cmp()` across all three `change_attr_type` modes: monotonic (newer / unchanged / stale), strict monotonic (where equal counts as stale), undefined and missing change attributes returning "not sure", and the generation counter overriding the change attribute |
| `nfs-inode-cache-invalid` | `nfs_zap_mapping()` and `nfs_set_cache_invalid()`: the data-cache flag is skipped when no pages are cached and set when they are, validity flags accumulate rather than replace, and `NFS_INO_REVAL_FORCED` is never stored |
| `nfs-inode-cache-expiry` | `nfs_attribute_timeout()` jiffies window, a zero timeout expiring immediately, `nfs_check_cache_invalid()` honouring explicit flags, and a delegation suppressing expiry entirely |
| `nfs-inode-out-of-order` | `nfs_ooo_merge()` gap recording, merging of abutting ranges in both directions, disjoint ranges kept apart, empty ranges collapsing, and gap-table overflow falling back to `NFS_INO_DATA_INVAL_DEFER` with the table released |
| `nfs-inode-zap-caches` | `nfs_zap_caches()` invalidating data for regular files but withholding it for special files, and `nfs_invalidate_atime()` touching only the atime bit |
| `nfs-inode-helpers` | `nfs_fileid_to_ino_t()` folding rather than truncating 64-bit fileids, `nfs_get_valid_attrmask()` mapping cache validity to answerable statx fields, `nfs_file_has_writers()`, and `nfs_zap_acl_cache()` dispatching through its protocol hook |
| `nfs-inode-alloc` | `nfs_alloc_fattr()` starting invalid, `nfs_fattr_init()`/`nfs_fattr_set_barrier()` advancing the generation counter, `nfs_alloc_fhandle()` starting empty |
| `nfs-inode-update` | `nfs_update_inode()`'s identity guards: a changed fileid or changed file type is refused with `-ESTALE` and marks the inode stale, a mounted-on fileid explains an apparent mismatch, and a matching reply refreshes the revalidation timestamp |
| `nfs-inode-wcc` | `nfs_wcc_update_inode()` weak cache consistency: change attribute, mtime, ctime and size are each adopted only when the reply's "before" value matches what the inode holds, discarded when it does not, and size additionally withheld while writebacks are pending |
| `nfs-inode-refresh` | `nfs_refresh_inode()` treating an empty fattr as a no-op and propagating `-ESTALE` from the identity guards below it |
| `nfs-inode-check-attrs` | `nfs_check_inode_attributes()` flagging the right cache bit per changed attribute (size, mtime, change, mode, owner, nlink, atime), comparing only permission bits of the mode, enforcing the identity guards, and skipping everything under a delegation |
| `nfs-inode-post-op` | `nfs_post_op_update_inode()` invalidating directory data but not regular-file data, and setting an attribute barrier |
| `nfs-inode-update-body` | the application half of `nfs_update_inode()`: mtime/ctime/atime, size (including refusing to shrink under pending writebacks while still allowing growth), mode taking only permission bits, owner, nlink, and space-used converted to 512-byte blocks |
| `nfs-inode-setattr` | `nfs_setattr_update_inode()` applying mode and ownership, invalidating the access cache on chown, and installing an attribute barrier |
| `nfs-inode-timestamps` | `nfs_set_timestamps_to_ts()` storing explicit utimes values and clearing the matching cache bits, and `nfs_update_timestamps()` clearing ctime alongside mtime but leaving atime alone |
| `nfs-inode-partial-update` | `nfs_ooo_record()` capturing a change gap only when both halves are present, and `nfs_inode_finish_partial_attr_update()` accepting an unmoved change attribute while declining when the change attribute is itself invalid or nothing is outstanding |
| `nfs-inode-cache-match` | `nfs_find_actor()`, the predicate `iget5_locked()` uses to match a cached inode: each of its four rejection reasons (fileid, type, filehandle, staleness) tested separately, plus `nfs_init_locked()` seeding an inode that the same descriptor then matches |
| `nfs-inode-readdirplus` | `nfs_getattr_readdirplus_enable()` requiring server support, no pending writebacks, and an attribute timeout long enough for the extra data to still be useful |
| `nfs-inode-revalidate` | `__nfs_revalidate_inode()` error handling with a stubbed `getattr`: `-ESTALE` marking a regular file stale but only zapping a directory's caches, `-ETIMEDOUT` absorbed under `NFS_MOUNT_SOFTREVAL` and propagated without it, other errors passed through, and a known-stale inode short-circuited without a round trip |
| `nfs-inode-revalidate-gate` | `nfs_revalidate_inode()` skipping the round trip when the cache is valid, issuing one when the requested flag is invalid, reporting `-ESTALE` without querying, and `nfs_mapping_need_revalidate_inode()` |
| `nfs-inode-sync` | `nfs_sync_inode()` and `nfs_commit_inode()` on a clean inode, commit-counter balance across repeated calls, and `nfs_sync_mapping()` short-circuiting with no cached pages |
| `nfs-inode-lifetime` | `nfs_drop_inode()` dropping a stale inode even when the generic rules would keep it, `nfs_fattr_fixup_delegated()` stripping server timestamps under a delegation but keeping ones the client has already marked invalid, and `nfs_file_has_buffered_writers()` excluding O_DIRECT files |
| `nfs-inode-lock-context` | `nfs_init_lock_context()` starting referenced and idle, and `__nfs_find_lock_context()` matching on the current task's file table while skipping contexts owned by another |
| `nfs-inode-open-context` | `get_nfs_open_context()` refusing a context whose count has reached zero, `nfs_inode_attach_open_context()` linking to the inode and invalidating data only when out-of-order gaps are outstanding, and `nfs_find_open_context()` matching on credential, exact access mode and open state |
| `nfs-inode-ooo-state` | `nfs_ooo_test()` distinguishing a deferred invalidation and recorded gaps from an allocated-but-empty gap table, and `nfs_clear_inode()` dropping ACL validity |
| `nfs-inode-wait-bit` | `nfs_wait_bit_killable()` signal semantics per wait mode: interruptible aborts on any signal, uninterruptible ignores signals entirely, killable ignores a non-fatal one, and an exiting task returns `-EINTR` before scheduling at all |
| `nfs-inode-pagecache` | `nfs_vmtruncate()` shrinking, clearing size invalidity, dropping data invalidity and out-of-order gaps only when truncating to zero, refreshing mtime under a delegation but not without one, and rejecting negative or over-limit sizes; `nfs_invalidate_mapping()` with nothing cached |

`nfs_vmtruncate()` is covered for its bookkeeping but **not** for its
namesake. Every branch of the size check, the validity flags and the
delegated-mtime update is exercised, but `truncate_pagecache()` is only
ever called on an empty mapping, so no page is ever actually dropped.
The function's own logic is tested; the truncation it performs is not.

### Testing the page cache for real

These tests put actual folios in the page cache and check that truncation
and invalidation remove them, rather than only exercising the
empty-mapping fast paths. Three pieces make that possible:

- `address_space_init_once()` is exported, and sets up the xarray, the
  `i_mmap` root and the locks.
- `filemap_add_folio()` inserts a real folio, so `nrpages` becomes
  non-zero as a *consequence* rather than as a claim.
- `mapping->a_ops` must point somewhere. The page cache dereferences it
  unconditionally in places -- `filemap_free_folio()` reads
  `a_ops->free_folio` before testing it -- so the kernel's own
  `empty_aops`, the all-NULL table `inode_init_always()` installs, is
  used.

A plain folio carries no private data, so `folio_needs_release()` is
false and `truncate_cleanup_folio()` never reaches
`a_ops->invalidate_folio`. That is why no filesystem-specific operations
are needed.

Two earlier attempts panicked, and both were the fixture's fault rather
than a limitation:

1. Setting `nrpages` by hand on an empty mapping. The code believed there
   was data to flush, reached `filemap_write_and_wait_range()` and
   dereferenced a NULL `a_ops`.
2. Adding real folios but leaving `a_ops` NULL, which crashed in
   `filemap_free_folio()` during truncation.

The rule both illustrate: **a fixture may leave things out, but it must
not describe a state the kernel cannot produce.** A zeroed pointer the
code tests for is fine; a count that contradicts the structure it
describes, or an absent vtable the code dereferences unconditionally, is
not.

`nfs_wait_bit_killable()` looked untestable because it calls
`schedule()`. It is not: `schedule()` only blocks when the task state is
something other than `TASK_RUNNING`, and the wait_bit machinery sets that
state *before* invoking the action function. Called directly from a test
the state is still `TASK_RUNNING`, so `schedule()` yields and returns.
That leaves the signal handling reachable, which is the half worth
testing. `TIF_SIGPENDING` is set and cleared around each call so nothing
leaks into the rest of the run.

`get_nfs_open_context()` is guarded by `refcount_inc_not_zero()`, so a
context already being torn down is refused rather than resurrected;
getting that wrong would hand out a freed context. The open-context tests
need a dentry, but only for its `d_inode` and `d_sb` pointers, so a
zeroed struct with those two fields set is enough.

`nfs_fattr_fixup_delegated()` has the subtler rule of the three: a
delegation makes the client's timestamps authoritative, so server values
are discarded -- but only for times whose caches are still believed
valid. A timestamp the client has already marked invalid survives,
because the delegation is not a substitute for knowledge the client has
admitted it lost.

`nfs-inode-sync` is deliberately shallow and worth flagging as such. Each
step of `nfs_sync_inode()` has a cheap exit when nothing is outstanding:
`inode_dio_wait()` returns immediately with `i_dio_count` at zero,
`filemap_write_and_wait()` skips writeback when `nrpages` is zero, and the
commit loop terminates at once on an empty commit list. That makes the
clean path reachable, and these tests pin it -- a regressed guard would
hang here rather than return.

What remains genuinely unreachable is the case that matters: actually
flushing dirty pages needs real page-cache state, and no amount of
struct-filling substitutes for it.

### Talking to a "server" without one

`__nfs_revalidate_inode()` reaches the wire through
`NFS_PROTO(inode)->getattr`, which is a function pointer in the
`nfs_rpc_ops` vtable. A stub standing in for it gives complete control
over what the server "returns", which is the whole point: the interesting
logic in this function is entirely in its error paths, and those are the
ones that are awkward to provoke against a live server. A soft-timeout
being converted to success, or `-ESTALE` being handled differently for a
directory than a regular file, are one-line branches that a functional
test would have to work hard to reach.

The same vtable is the seam behind the delegation and ACL stubs elsewhere
in this file. It is worth stating plainly: in `fs/nfs`, protocol
operations are indirect calls, so "needs a server" is almost never the
real obstacle.

`nfs_find_actor()` is worth singling out: a false positive there hands back
the wrong inode entirely, and the filehandle check is what catches two
different files that happen to share a fileid across filesystems. The
callback is pure, so it can be driven directly without involving the inode
cache at all.

The `nfs-inode-check-attrs` cases all start from an inode and a reply that
agree in every respect and then perturb exactly one attribute, so a
validity bit appearing anywhere else is a leak between comparisons rather
than the behaviour under test.

Weak cache consistency is the subtlest of these. A server reply can carry
an attribute both before and after an operation; if the "before" value
matches what the client already holds then nothing else touched the file
in between and the "after" value can be adopted without a fresh GETATTR.
If it does not match, applying it anyway would silently overwrite another
client's change. Each attribute is gated independently, so the tests check
both directions per attribute rather than only the happy path.

`kunit/pnfs_test.c` covers pNFS layout range arithmetic in `fs/nfs/pnfs.h`:

| Suite | Covers |
|---|---|
| `pnfs-end-offset` | `pnfs_end_offset()` saturating at `NFS4_MAX_UINT64` rather than wrapping, both edges of the cap, and that the end never precedes the start |
| `pnfs-range-intersect` | half-open overlap including the touching-boundary case, unbounded ("to end of file") ranges, symmetry of the predicate, and the counter-intuitive treatment of zero-length ranges |

These are `static inline` in a private header, so unlike `inode.c` nothing
needs un-staticing — the test only has to live in `fs/nfs` to include it.

One finding worth recording: a zero-length layout range does **not**
intersect itself, but **does** intersect any range that strictly straddles
its offset, because the predicate is `start2 < end1 && start1 < end2`. The
test asserts that behaviour rather than the intuitive "empty intersects
nothing", which is what it was originally written to expect.

`nfs_inode_attrs_cmp()` decides whether attributes in an RPC reply are
newer than what the inode holds. RPC replies can be reordered, so a stale
reply overwriting fresh attributes is cache corruption that is very hard to
reproduce deliberately — exactly the kind of thing worth pinning in a unit
test rather than hoping an integration run trips over it.

### What it costs to test a file like inode.c

Two things, and they are the general answer for `fs/nfs`:

1. **The function is file-private.** The runner's `UNSTATIC` list applies
   the kernel's own `VISIBLE_IF_KUNIT` / `EXPORT_SYMBOL_IF_KUNIT`
   (`include/kunit/visibility.h`) to it, which drops the `static` only when
   `CONFIG_KUNIT` is set and puts the symbol in a test-only namespace. This
   is the mechanism upstream uses for `gss_krb5_crypto.c`, not a
   workaround. It does mean the runner edits the file under test, so that
   edit is grep-guarded like the rest.
2. **It needs a `struct inode`.** That needs a `struct super_block`, which
   needs a `struct nfs_server` on `s_fs_info`. None of them need to be
   real: no mount, no VFS registration, no server. Three zeroed structs
   with three fields filled in is enough, because `NFS_I()` is a
   `container_of` and `NFS_SERVER()` is a pointer chase. That fixture is
   about thirty lines.

The limit is what the function *does*, not which file it lives in, and it
is looser than it first appears. `nfs_zap_mapping()` and
`nfs_set_cache_invalid()` were initially written off here as needing a
working page cache. They do not: both only read `mapping->nrpages` as a
count, so a zeroed `struct address_space` with that one field set is
enough. `nfs_have_delegated_attributes()` looked like another blocker and
turned out to be a seam, since it dispatches through
`NFS_PROTO(inode)->have_delegation` — a function pointer, so a three-line
stub replaces the whole delegation subsystem.

The fixture grew from three structs to six (adding `nfs_client`,
`nfs_rpc_ops` and `address_space`) and gained a `spin_lock_init()`. That is
the entire cost of moving from pure comparison logic to functions that take
inode locks and consult the page cache.

What genuinely remains out of reach is narrower still: functions that
*issue RPCs* or *wait*, where there is no seam to stub and no substitute
for a server. Reaching those means xfstests or pynfs, not more scaffolding.

Two behaviours there are worth calling out because they are security- rather
than correctness-shaped: opaque padding must be zero-filled, since
uninitialised padding would put kernel memory on the wire, and a short
`uint32` array must zero the unused tail of the caller's buffer rather than
leave stale values. Both are tested by pre-filling with `0xff` and checking
the codec scrubs it.

`kunit/timer_test.c` covers `net/sunrpc/timer.c`, the Van Jacobson RTT and
variance estimator used for RPC over datagram transports, plus the
`rpc_set_timeo()`/`rpc_ntimeo()` inlines in
`include/linux/sunrpc/timer.h`:

| Suite | Covers |
|---|---|
| `sunrpc-rtt-init` | `rpc_init_rtt()` seeding, including the pre-scaling of timeouts above `RPC_RTO_INIT` |
| `sunrpc-rtt-update` | discarding wrapped (negative) samples, treating a zero sample as 1, convergence of `srtt` to 8× a constant RTT, the variance floor, per-request-type slot independence |
| `sunrpc-rtt-rto` | `timer == 0` falling back to the default, mean-plus-deviation arithmetic, upward rounding, clamping to `RPC_RTO_MAX` |
| `sunrpc-rtt-ntimeo` | the timeout counter: clamping at 8, decay-by-one on improvement rather than reset, and the `timer == 0` special case |

Note the timer index convention both files depend on: `rpc_update_rtt()`
and `rpc_calc_rto()` each do `timer--`, so caller-visible timer 1..5
selects slot 0..4 and timer 0 means "not a frequently issued RPC".

The IPv6 shorthands `addr.c` special-cases are covered deliberately, since
special cases are where formatting bugs hide: `::`, `::1`,
`::ffff:192.168.1.1`, compressed form, and the rule that a scope id is
appended only for link-local addresses and only when non-zero.

`scripts/kunit/run-sunrpc-kunit.sh` copies each file listed in its `TESTS`
array from `kunit/` into its target directory under the kernel tree
(`net/sunrpc/`, `fs/nfs_common/` or `fs/nfs/`) and adds the Kconfig, Makefile and
`.kunitconfig` wiring for it. `./linux` is gitignored, so the test
sources live in this repo rather than in the kernel tree. Every edit is
grep-guarded, so re-running after a re-fetch re-wires the tree
automatically. Adding another suite means dropping the file in `kunit/` and
adding one line to that array.

`CONFIG_IPV6=y` is added to `.kunitconfig` because the stock file does not
set it, and without it `rpc_pton6()`/`rpc_ntop6()` compile to stubs that
return 0 — every IPv6 case would pass while testing nothing.

## xfstests cases that ARE ported: generic/* over a loopback NFS mount

The `kunit/xfstests/` tree holds ports of **45 xfstests generic cases**,
each a KUnit suite named after its original (`xfstests/generic/001` ...),
each running against a real NFS mount served by knfsd inside the same UML
kernel. The deployment lives in `kunit/xfstests/nfs_fixture.{c,h}`: tmpfs
on `/export` (size settable per suite for the ENOSPC family), knfsd
v4-only on 127.0.0.1:2049, the real client mounted as NFSv4.2 on
`/mnt/nfs`. mountd's three caches are fed directly; nfsdfs is mounted
because `create_client()` needs it; grace is ended the `v4_end_grace` way.
Bring-up is refcounted per suite, so every full run also exercises ~60
consecutive nfsd restart and mount/unmount cycles.

Ported: 001 002 005 006 007 011 013 014 020 023 028 029 030 035 037 069 070 074 075 087 088 089 109 123
126 129 131 132 169 193 213 221 228 236 245 257 258 285 286 306 308 309
313 314 360.

Two rules bound the set. A case upstream reports `[not run]` on an NFSv4.2
mount is not ported, since there is no upstream result to mirror; measured
with `scripts/00-run-xfstests-on-vm-and-docker.sh` against knfsd in docker
at `vers=4.2`. And a case whose subject is a userspace library rather than
the filesystem is not ported either -- generic/010 drives ndbm through
`src/dbtest`, which has no in-kernel equivalent to mirror.

Each port is meant to perform upstream's operations and assert upstream's
outcome, at reduced scale where the original's magnitudes do not fit an
in-kernel tmpfs export, and single-threaded where the original forks. Where
that reduction loses the point of the test, the port says so in its header;
where upstream keeps an NFS-specific golden image (`035.out.nfs`), the port
follows it rather than the default one.

The families: protocol-pin mirrors for ops NFS lacks (021 collapse, 058
insert, 092 bare KEEP_SIZE, 024 renameat2 flags, 110 clone-on-tmpfs, 004
O_TMPFILE); namespace semantics (023, 035 sillyrename-on-rename-over, 089
mtab link/rename churn, 109, 245, 294, 309, 360); data integrity (001
chain copier, 075 mini-fsx with a shadow model, 069 O_APPEND, 071, 074,
129, 132, 169, 213 ALLOCATE boundaries, 255 punch matrix, 286 seek-driven
sparse copy, 308 1TB offsets); ENOSPC on a 16MB export (015, 102, 204,
273, 275, 320); timestamps (221, 236, 258 pre-epoch, 313); xattrs -- RFC
8276 works end to end here -- (020, 037, 062, 070 model-checked storm,
097); permissions via in-kernel credential switching with dropped
capabilities (087, 088, 123, 126, 193, 314 SGID inheritance); plus POSIX
locks as NFSv4 LOCK state (131), SEEK RPCs (285/286), READDIR cookie
stability (257), RLIMIT_FSIZE (228) and device nodes on RO mounts (306).

NFS-specific semantics the porting surfaced and pinned, each found as a
failing "wrong" expectation and verified before being encoded:

- RENAME_NOREPLACE is two-layer: EEXIST from the VFS's exclusive lookup
  when the target exists (works over NFS with no protocol support), EINVAL
  from nfs_rename once past it (024).
- EEXIST-vs-EROFS on a read-only mount depends on the dcache: primed
  names give EEXIST, cold lookups take nfs_lookup's exclusive-create
  shortcut, never ask the server, and yield EROFS (294).
- A same-size truncate is optimised away by the client -- no SETATTR, no
  ctime/mtime update -- diverging from local filesystems (313).
- Renaming over an open target sillyrenames it: nlink stays 1 and a .nfs
  entry appears until the last close (035); removal storms can leave
  transient .nfs entries, hence the fixture's settled rmdir (011/013).
- Space freed by REMOVE returns eventually, not immediately (server-side
  file caching): the ENOSPC ports wait, bounded (015/102/204).
- statfs over NFS reports f_bsize as the 128K transfer size, not the
  filesystem block size -- unit bugs in tests are easy (015/102/204).
- In-kernel opens lack force_o_largefile(): without O_LARGEFILE the 2GiB
  MAX_NON_LFS limit applies (308).
- xattr gets are served from the client's xattr cache; only a server-side
  check (through the export directory) proves the SETXATTR wire value
  (097 -- added after a truncation mutation went uncaught).
- `common/punch`'s engine is shared by four collapse tests that differ
  only in flags: 021 plain, 022 `-d` (no fsync), 012 `-k`, 016 `-d -k`.
  All four are now ported, because the flags are not cosmetic:
  - `-k` keeps the scratch file between the 17 layouts, so each is built
    on the previous one's result. That is the whole difference between 012
    and 021, and upstream's golden files differ because of it. 012.c and
    016.c are cumulative to match, and assert the carryover positively
    (an offset a layout never wrote must still hold earlier data) --
    a shadow-model test would otherwise pass either way.
  - `-d` drops the fsync, so the layouts reach the server only via
    writeback. 016.c/022.c therefore double as close-to-open consistency
    tests, and are the only ports that exercise it: mutating
    `nfs4_file_flush()` to return early fails both on the first case that
    writes anything, while 012 (same layouts, with fsync) still passes.
  Getting that mutation to bite took three attempts, and each failure was
  informative: `nfs_getattr()` flushes when STATX_CTIME/MTIME are asked
  for (inode.c:982), `nfs_file_read()` flushes before invalidating a
  mapping, and the v4 mount's `.flush` is `nfs4_file_flush()`
  (nfs4file.c:111) -- not `nfs_file_flush()` (file.c:140), which serves
  v2/v3 only. Hence the strict ordering in those ports (server check
  first after close, size and client reads afterwards) and the permanent
  per-case log line reporting whether the server had the data before the
  close.

The 020-030 band is now complete.

An earlier version of this section claimed 029 and 030 were both impossible,
because `vm_mmap()` needs `current->mm` and a KUnit case runs in a kernel
thread which has none. **That was wrong, and it was wrong by not looking**:
KUnit ships `kunit_vm_mmap()` (`lib/kunit/user_alloc.c`), which allocates an
mm, runs `arch_pick_mmap_layout()` on it, attaches it with
`kthread_use_mm()`, and tracks the mapping as a test resource -- `mm_alloc()`
is even already `EXPORT_SYMBOL_IF_KUNIT` for the purpose, and the helper is
built into `lib/kunit` unconditionally. So **mmap is available to every
port**, and 029 and 030 are both in. Writes into a mapping go through
`copy_to_user()`, which is the correct way to touch user addresses with a
borrowed mm (a bare dereference happens to work on UML but not under SMAP or
PAN).

**030** was then also recorded as out, for a smaller and more specific
reason: it drives `mremap` around its truncates, and `mremap` exists only as
a syscall entry point (`SYSCALL_DEFINE5(mremap, ...)`) with only static
helpers. `nm` on `.kunit/vmlinux` confirms it -- `__do_sys_mremap`,
`__se_sys_mremap` and `sys_mremap` are all local symbols (`t`, not `T`), so
nothing outside `mm/mremap.c` can call it, and there is no `vm_mmap()`
equivalent.

**That reason was real but the conclusion was still wrong, because the
mremap calls do nothing.** `mremap` rounds both lengths up to a page, and
030's file is 5017k, which is 1254.25 pages. `PAGE_ALIGN(5017k)` and
`PAGE_ALIGN(5020k)` are both 5020k, so every `mremap -m 5020k` / `mremap
5017k` in upstream 030 takes the `old_len == new_len` path and returns the
same address without touching a VMA. What they resize is xfs_io's own record
of the mapping length, which is what lets its next `mwrite` clear its own
bounds check. A 5017k mapping already covers the whole range 030 writes to.

This was established by doing it the wrong way first: a forwarding wrapper
was appended to `mm/mremap.c`, the grow and shrink were performed through it,
and an assertion that a write past the shrunk mapping must now fail was added
to prove the shrink had landed. It did not fail -- the tail was still mapped.
The wrapper and the kernel edit were removed; a kernel change to call a
function that provably does nothing is worse than no test. `kunit/xfstests/
generic/030.c` asserts the rounding directly instead, so if the premise ever
stops holding the test says so rather than silently drifting.

What 030 does add is a second layout over 029's code path: unaligned mapped
writes inside the last page of a ~5 MB file, versus 029's page-multiple 5 KB
ones. It is worth being precise about how much that is worth, because two
mutations run against both give a split answer:

| mutation | 029 | 030 |
|---|---|---|
| drop `truncate_pagecache()` in `nfs_vmtruncate()` (inode.c:811) | catches | catches |
| drop `nfs_folio_length()`'s partial-last-folio clamp (internal.h) | catches | **misses** |

The clamp mutation is caught by 029 because its third case is 5121 bytes, and
is invisible to 030 -- including to the mid-test check described below. I
predicted twice that 030 would catch it and was wrong both times, so the
measurement stands without a third guessed mechanism. **030 is not a strictly
stronger 029.**

030's one genuine improvement on upstream is where it looks. Upstream dumps
the file only at the end, by which point its final `mwrite Y` has overwritten
the entire range the truncates disturbed. The port adds a check between the
truncate up and the Y write, and that is the assertion the
`truncate_pagecache()` mutation fails on -- "byte 5137408 is 57, expected
00", the stale W surviving the truncate down, on both scenarios. The
end-of-test comparison upstream relies on does not notice.

029 covers a path no other port reaches, which its mutations confirm:
making `nfs_vm_page_mkwrite()` skip recording the dirty range loses every
mapped write and fails all three of its scenarios while every other suite
stays green; and making `nfs_vmtruncate()` skip `truncate_pagecache()` leaves
stale bytes past the new EOF, which 029 catches both server-side and
client-side ("byte 5118 is 58, expected 00") alongside the older
nfs-inode-pagecache unit tests and 014/075.

What the newly ported four added, and what porting them taught:

- **025** runs `_rename_tests`' real 5x5x2 type matrix for RENAME_EXCHANGE,
  where the already-ported 024 only probed one combination. The matrix
  splits into two regimes -- either name absent gives ENOENT from
  do_renameat2()'s own checks and never reaches NFS; both present gives
  EINVAL from nfs_rename() -- so 024's single probe was exercising just one
  of them. Teeth confirmed: making nfs_rename() accept flags fails 024 and
  025 together.
- **026** pins that POSIX ACLs are unusable over NFSv4 (no handler; and the
  tmpfs export is built without CONFIG_TMPFS_POSIX_ACL) and that the NFSv4
  ACL xattr refuses cleanly -- measured EOPNOTSUPP, reported rather than
  hard-pinned since an ACL-carrying export would answer differently.
  Mutation established a limit worth writing down: giving the nfs4_acl
  handler the POSIX name so the call reaches the wire leaves the test
  passing, because the server refuses with the same errno. The test pins
  the user-visible contract, not which layer refuses, and now says so.
- **027** fills eight directories round-robin to ENOSPC on a 16MB export,
  four times, checking a 2MB reserve file survives each squeeze. Two
  findings: the reserve check had to move server-side (a client-side read is
  answered from the page cache), and 027 detects ENOSPC at *file creation*
  rather than at write/fsync -- the fsync-swallowing mutation that fails
  015/204/273/275 leaves 027 green. It is the only ENOSPC port covering the
  create path, and no one-line kernel mutation for it has been found yet.
- **028** is upstream's getcwd() race, rendered as d_path() over a churning
  tree including a renamed ancestor. Teeth confirmed: a nfs_rename() that
  returns success without telling the server fails it.

### The 031-039 band

031, 032, 033, 034 and 039 are ported; **036 and 038 are not, and will not
be.** Both need things the fixture cannot have and that no amount of
restructuring supplies:

- **036** is CVE-2014-8086, an aio/dio race. It runs the compiled
  `aio-dio-fcntl-race` binary, which needs libaio, multiple userspace
  threads, and `fcntl()` toggling `O_DIRECT` underneath in-flight AIO. There
  is no AIO submission path callable from a KUnit case, and the race is
  between userspace threads by construction.
- **038** stresses btrfs block-group allocation against `fstrim` running in
  parallel, over 200,000 files. tmpfs has no discard, NFS has no trim
  operation, and the bug is in btrfs' block group lifecycle -- there is no
  client-side residue to test.

Of the five that are in, two are honest partial ports and the docs should not
pretend otherwise. **034 and 039 are both dm-flakey crash-consistency tests,
and the crash is the test.** dm-flakey needs a block device; the fixture
exports tmpfs and runs client and server in one kernel, so writes cannot be
dropped and replayed. Neither port tests what upstream tests, and **nothing in
the ported set covers crash consistency at all** -- it is the largest single
gap in this collection.

What those two keep is still NFS-specific rather than filler. Both upstream
tests end by unlinking every entry and calling rmdir, and over NFS that is
where sillyrename bites: an unlinked file whose `struct file` still awaits its
delayed fput becomes a `.nfsXXXX` entry, the directory is not empty, and rmdir
returns ENOTEMPTY -- the same errno as the btrfs bug, from an unrelated cause.
Both ports therefore use the **plain** `xfs_rmdir()`, not
`xfs_rmdir_settled()`, so that condition is reported rather than retried away.
034 additionally covers directory fsync (`nfs_fsync_dir`), which no other port
calls; 039 asserts nlink at each step through a forced revalidation, since a
stale cached nlink would pass a test that only checked the file still exists.

The other three:

- **031** is generic/012's collapse-refusal in a second layout: two
  overlapping writes whose offsets and lengths are not page multiples (55756
  bytes at 185332, 63394 at 133228), where 012's layouts are all 64K units.
  Same relationship 030 has to 029 -- a layout, not a mechanism. Its golden
  output inverts against upstream's, because upstream's expected size (196032)
  is what the file measures after two successful collapses remove 45056 bytes,
  and here they are refused.
- **033** is thinner still, and labelled as such in the source: NFSv4.2 has no
  ZERO_RANGE, so all sixteen `fzero` calls return EOPNOTSUPP and the file keeps
  its data. Upstream expects 64K of zeroes; this expects 64K of 0xcd. Its
  value is forward-looking -- if this ever stops returning EOPNOTSUPP, something
  is emulating ZERO_RANGE client-side, and the byte check says whether the
  emulation is right.
- **032** is the one with genuinely new coverage. Its fiemap and
  unwritten-extent assertions cannot be ported (neither concept reaches an NFS
  client), but what remains is **the only case in the set with a second thread
  inside the NFS client at the same time as the writer**: a background loop
  calling `sync_filesystem()` on the NFS superblock while sub-page writes, a
  real ALLOCATE, a 1 MB overwrite and an fsync run against the same file. It
  is a check on locking rather than on sequencing. The case logs its sync-loop
  count and fails if it is zero, so a pass cannot silently mean the
  concurrency never happened.

### The 040-049 band

Ported: **040, 041, 047, 048**. **049 is folded into 048** as a second case,
not given a suite of its own. **042, 043, 044, 045, 046 are not ported.**

This band is dominated by two upstream families, and both lose their core to
the same missing capability:

- **040 and 041** are dm-flakey crash tests, like 034 and 039. No block
  device, no crash, no log replay.
- **043-049** are the "NULL files problem" family. Every one of them calls
  `_scratch_shutdown` (the XFS shutdown ioctl, `src/godown`) and then counts
  extents with fiemap. NFS has neither: there is no shutdown ioctl, and
  fiemap is not in NFSv4.2, so "non-zero size but no extents" is a question
  the client cannot ask.

**Crash consistency remains entirely uncovered by this collection.** Four
ports now sit in its shadow (034, 039, 040, 041) and none of them test it.

What the four ports keep:

- **040** reduces to link-count bookkeeping at scale, which over NFS is a
  protocol question rather than an on-disk one -- nlink travels in GETATTR and
  is cached on the client inode. Upstream's entire output is two link counts
  and the file's contents, and that is exactly the part that survives:
  `N + 2` after the links are made, `1` after the bulk unlink, both read with
  a forced revalidation, then the data checked on the server so a surviving
  inode is proven reachable rather than ESTALE.
- **041** is the one with content 040 does not have. It removes a link and
  then **recreates a link under the name it just removed** before fsyncing.
  Over NFS that is a dentry-cache question: the client must not serve the
  removed name from a stale dentry, nor hide the recreated one behind a
  negative entry. The port keeps upstream's name-by-name sweep including its
  inverted check for the single index that stays removed, because a link
  count alone cannot see either failure.
- **047** is per-file `fsync` in bulk -- `nfs_file_fsync()` -> `nfs_wb_all()`
  plus COMMIT, across many files rather than one. It is the only case in the
  set shaped that way, so a client that dropped one COMMIT among hundreds
  fails here and nowhere else.
- **048** is the same durability question through `sync_filesystem()` on the
  whole superblock, which is a different path from 047's per-file fsync and
  the only place in the set where syncfs is the durability mechanism.

**On folding 049 in.** 048 syncs as it writes; 049 writes everything unsynced
and syncs once at the end. Upstream keeps them apart because the XFS log
replay paths they expose *after a shutdown* differ -- and without a shutdown
that divergence does not exist, so over NFS they land on the same code path. A
separate 049 suite would be a copy of 048 with one loop moved. Both shapes run
as cases of 048; the distinction upstream draws is real, and this deployment
simply cannot see it.

The five that are out, individually:

- **042** needs `src/godown` plus a loopback-mounted filesystem image inside
  the scratch mount, and detects stale data by pre-writing a pattern to the
  *image*. There is no image and no block layer here, and its three operations
  (`falloc -k`, `fpunch`, `fzero -k`) are two that NFS rejects outright.
- **043, 044, 045, 046** are shutdown-plus-fiemap, as above. Strip both and
  what remains is "write files, optionally truncate, check size and content"
  -- 045 is write-64K-truncate-to-32K and 046 is write-32K-truncate-to-64K,
  which is truncate-down-discards and truncate-up-zero-fills, already covered
  by 012, 029 and 030. Porting them would add four near-identical suites and
  no coverage, so they are declined rather than padded in.

### An intermittent whole-run live-lock (pre-existing, unresolved)

A full run sometimes never finishes. The UML process spins at ~99% CPU
indefinitely and no further KTAP output appears. Established so far:

- It always stalls **after every xfstests suite has passed**, somewhere in the
  pure-logic SunRPC suites that follow (observed stopping after
  `sunrpc-rtt-init`, after `sunrpc-addr-uaddr`, and after
  `sunrpc-rtt-ntimeo` on three different runs). Those suites are integer
  arithmetic and string parsing with no I/O, so they are almost certainly the
  victim rather than the cause -- something is holding the single CPU and they
  never get scheduled.
- **Every suite passes in isolation**, including the ones it stalls in:
  `sunrpc-rtt-*` alone runs in 0.068s, `xfstests/generic/032` alone in 1.2s,
  the whole `xfstests/generic/04*` band in 4.5s.
- **It is not caused by the 031-034/039/040/041/047/048 additions.** Removing
  all nine registrations *and* deleting their sources and `fs/Makefile` lines
  reproduces the hang on a 71-suite run.
- **It is not memory pressure.** `--kernel_args mem=2G` does not fix it.
- The host is not the problem: 10 CPUs, load 1.00 from the single spinning
  UML, ~2.7 GB available.
- It is intermittent -- the same runner completed full 806-test runs several
  times the same day.

The leading hypothesis, untested, is an accumulating leak in the fixture's
nfsd start/stop path: a full run now performs ~80 consecutive
`nfsd_svc()` bring-ups and mount/unmount cycles, and a leaked kernel thread
spinning after some cycle count would produce exactly this signature. That is
a guess, not a finding, and it is recorded as one. Diagnosing it properly
needs a console on the wedged UML to see what the runnable task is.

Practical impact: **CI can report a hang rather than a failure**, and a hung
run produces no totals line at all. When that happens, re-run; if a specific
result is needed, a filtered run (`kunit.py ... "xfstests/generic/04*"`)
completes reliably.

### A note on green results and kernel logs

generic/032's background syncer originally called `sync_filesystem()` without
holding `s_umount`. The case reported PASSED while emitting **404 WARNs** in a
single run -- three per sync loop, from `fs/sync.c:38` and two places in
`sync_inodes_sb()` (`fs/fs-writeback.c:2626` and `:2803`), each of which opens
with `WARN_ON(!rwsem_is_locked(&sb->s_umount))`. `SYSCALL_DEFINE1(syncfs)`
takes that lock around the same call; the thread now does too, as does
generic/048's `g048_syncfs()`.

The lesson is worth keeping: **a green KUnit result says nothing about what
the kernel logged underneath it.** Nothing in the runner fails a suite for
WARNing, and the default (non-raw) `kunit.py` output does not show kernel log
lines at all -- the warnings were only visible because a `--raw_output` run
was being read for another reason. The same run also confirmed the syncer is
genuinely concurrent with the writer: 134 sync loops interleaved with the
10 write iterations.

Two debugging notes from this batch, both costing several rounds:
`nfs_update_folio()` rounds a write's dirty range back up to the page
boundary, so the "drop the last byte" mutation is absorbed entirely for
page-aligned writes -- which is why it fails the unaligned ports and not
012/027. And 025 spent three rounds chasing a phantom "partially applied
exchange" that was really its own cleanup leaking a directory: removing the
"tree" layout's child leaves a sillyrename entry pending, so a plain rmdir
returns ENOTEMPTY. The fix is `xfs_rmdir_settled()` plus an assertion at the
cleanup itself, so the next such leak is reported where it happens rather
than three combinations later. Worth noting the failure mode: mid-hunt I
"corrected" a expectation that was right all along to match a measurement
that was an artifact of that leak.

Validation on the full set: three one-line kernel mutations -- the client
write path dropping a byte (17 failures across the data ports), rename
silently skipping its RPC (8 failures across the namespace ports), and
SETXATTR truncating its wire value (caught precisely by 097's server-side
check) -- each reverted to a double-confirmed green run. Whole-run cost of
all 69 ports plus fixture cycles: under two minutes wall clock including
the kernel build.

## Why these are not ports of xfstests cases

An obvious-sounding idea is to reimplement xfstests cases as KUnit tests.
It does not work, and the first ten `generic/` tests show why concretely:

| Test | What it exercises |
|---|---|
| `generic/001` | `creat`/`write`/`unlink` chains, checked for data corruption |
| `generic/002` | `st_nlink` after `link()` |
| `generic/003` | `noatime`/`relatime`/`strictatime`/`nodiratime` mount options |
| `generic/004` | `O_TMPFILE` opens, linked back into the namespace |
| `generic/005` | symlink `ELOOP` limits |
| `generic/006` | filename permutations |
| `generic/007` | `open`/`unlink`/`stat` errno consistency |
| `generic/008`, `009` | `fallocate` zero-range page boundaries |
| `generic/011` | directory stress |

Every one is a syscall- and VFS-level behaviour test. KUnit runs inside the
kernel with no mounted filesystem, no userspace process issuing syscalls,
and for NFS no server and no network. There is no function to call that
answers "does `relatime` suppress this atime update" — that behaviour *is*
the system: VFS, NFS client, XDR, RPC and server together. A KUnit test
named after `generic/003` would be a test in name only.

The survey was repeated over the next hundred `generic/` tests
(`generic/012` through `generic/111`) with the same result: none have a
unit-testable core. Their group tags show why — `metadata` (33), `log`
(23, journal replay after a crash), `fiemap` (22), `rw` (21), `prealloc`
(20), `shutdown` (13), `stress` (11). Every category needs a mounted
filesystem and real I/O.

What xfstests *is* good for here is pointing at the layer underneath.
`generic/007` checks that `open`/`unlink`/`stat` return sensible errnos end
to end, and `fs/nfs_common/common.c` is the pure lookup deciding what those
errnos are; the `generic/008`/`009` page-boundary cases have their analogue
in XDR's 4-byte alignment rule. Neither is a port. Both are the unit-level
layer beneath a system-level concern, which is how every file here was
chosen.

## A correction: fs/nfs is not wholly untestable

An earlier version of this document, and the planning behind it, claimed
that `fs/nfs` is not unit-testable because it is entangled with the VFS.
That is true of most of it and false as a blanket statement.

Of the 64 `.c` files in `fs/nfs`, several reference no inode, dentry, page
or file at all — among them `fs_context.c` (mount option parsing, 1684
lines), `nfs42xdr.c` (1674), `callback_xdr.c` (1141), `nfs4session.c` (657)
and `mount_clnt.c` (539).

`kunit/nfs4session_test.c` exists to settle the point concretely. NFSv4.1
session slot tables are bitmap allocation plus a control loop, need no I/O
and no server, and their whole API is exported through
`fs/nfs/nfs4session.h`. They are also squarely inside "NFSv4 state", which
the same earlier claim listed as untestable.

What remains genuinely out of reach for KUnit is narrower than first
stated: anything requiring a mounted filesystem, a socket, an RPC round
trip, or a live server. `fs_context.c`'s parsers are reachable too, though
they are `static` and would need `VISIBLE_IF_KUNIT` plus a constructed
`struct fs_context`.

## The UBSAN artefact, and a correction

With the stock `.kunitconfig` a run reports **215 passed, 2 failed**. With
`CONFIG_UBSAN=n` it reports **217 passed, 0 failed**:

```sh
kunit.py run --kunitconfig=net/sunrpc/.kunitconfig --kconfig_add CONFIG_UBSAN=n
```

The two "failures" are not test failures at all. The stock `.kunitconfig`
sets `CONFIG_UBSAN=y`, and under the UML build UBSAN reports a misaligned
access unrelated to any of this code:

```
UBSAN: misaligned-access in ../kernel/exit.c:774:2
member access within misaligned address ... for type 'struct task_struct'
which requires 64 byte alignment
```

KUnit attributes whatever lands in the log to whichever test happens to be
running, so the report surfaces as a failed case. Which case it lands on
shifts with binary layout: in the original 59-test baseline it appeared as
`64-fold("012345")`, and after adding more tests it moved to
`map NFSv2/v3 status.NFS_OK`. The report is emitted three times per run,
producing two attributed failures.

**This corrects an earlier claim in this document.** It previously recorded
`64-fold("012345")` and `Encrypt empty plaintext with
aes128-cts-hmac-sha256-128` as "two pre-existing upstream failures" at
`v6.12.57`. That was wrong. Both were this artefact; upstream's
`gss_krb5_test.c` passes in full. The mistake was reporting a red result
without checking whether it was an assertion failure or unrelated log
noise — the raw output shows a stack trace rather than an
`EXPECTATION FAILED` line, which is what distinguishes the two.

Whether the misaligned `task_struct` access is a genuine UML bug or a
false positive has not been investigated. It is unrelated to NFS.

## Verifying the tests can actually fail

A suite that cannot fail is worthless, and round-trip and convergence
tests in particular can pass vacuously. Four deliberate mutations were run
and all four were caught:

- claiming port 2049 encodes as `.8.2` rather than `.8.1` failed two
  independent cases, in *build* and in *parse universal address*
- putting the uncompressed form `2001:0db8:0:0:0:0:0:1` in the address
  table failed the formatting case, confirming the canonicalisation check
  is live rather than self-referential
- claiming `srtt` converges to 4× rather than 8× a constant RTT failed
  `update_converges_to_eight_times_rtt`
- claiming an improved timeout count is adopted outright rather than
  decayed by one failed `set_timeo_decays_by_one_on_improvement`

## Not covered

VAST's own multipath parser, `nfs_parse_port_group()`
(`vastnfs-4.5.8/bundle/fs/nfs/fs_context.c:785`), and
`rpc_calc_portgroup_offset()` (`bundle/net/sunrpc/clnt.c:651`). Both are
pure logic and both are flagged in
[vastnfs-vs-linux.md](vastnfs-vs-linux.md) as good KUnit targets, but they
cannot be tested this way today:

- the VM's stock Ubuntu kernel has `CONFIG_KUNIT is not set` and ships no
  kunit module, so no KUnit test module can load there
- the VAST out-of-tree build never sets `CONFIG_RPCSEC_GSS_KRB5_KUNIT_TEST`
  either (absent from `NFS_CONFIGS` in `vastnfs-4.5.8/makefile:197`), so
  even the existing KUnit test is not built by it
- `nfs_parse_port_group()` is `static`, so it would additionally need
  `VISIBLE_IF_KUNIT` (the header exists in Ubuntu's kernel headers)

Covering them needs a custom kernel built with `CONFIG_KUNIT=m`, which
would also invalidate the xfstests baselines taken on `6.8.0-138-generic`.
