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

Forty-three suites, 403 cases, across seven files.

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
