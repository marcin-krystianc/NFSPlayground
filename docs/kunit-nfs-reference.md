# KUnit tests for NFS: implementation reference

Working notes on how these tests are built: fixture mechanics, which seams
make each area reachable, per-case findings, and the constraints that
decide what can and cannot be tested this way. For what this is and how to
run it, see [kunit-nfs.md](kunit-nfs.md).

**Status: proof of concept.** Neither the unit suites nor the xfstests
ports are complete, and neither is meant to be read as coverage of the NFS
client. See [kunit-nfs.md](kunit-nfs.md#status) for the numbers.

## Runner mechanics

`scripts/kunit/run-nfs-kunit.sh` copies each file listed in its `TESTS`
array from `kunit/` into its target directory under the kernel tree
(`net/sunrpc/`, `fs/nfs_common/` or `fs/nfs/`) and adds the Kconfig,
Makefile and `.kunitconfig` wiring for it. `./linux` is gitignored, so the
test sources live in this repo rather than in the kernel tree. Every edit
is grep-guarded, so re-running after a re-fetch re-wires the tree
automatically. Adding a suite means dropping the file in `kunit/` and
adding one line to that array.

`CONFIG_IPV6=y` is added to `.kunitconfig` because the stock file does not
set it, and without it `rpc_pton6()`/`rpc_ntop6()` compile to stubs that
return 0 — every IPv6 case would pass while testing nothing.
`kunit/nfs4session_test.c` needs `CONFIG_NFS_V4=y` and `CONFIG_NFS_V4_1=y`,
neither of which is in the stock `.kunitconfig`; the runner adds both.

`CONFIG_KUNIT_ALL_TESTS=y` is what makes each generated `default
KUNIT_ALL_TESTS` stanza self-select. A side effect: upstream's own
`net/sunrpc/auth_gss/gss_krb5_test.c` is built and run too. It passes, and
it is not this repo's work -- so a run's Kerberos coverage is upstream's
crypto unit tests, not anything exercised over a mount here.

## The unit suites

Covered: `net/sunrpc/addr.c`, `timer.c`, `xdr.c`; `fs/nfs_common/common.c`;
and `inode.c`, `nfs4proc.c`, `nfs4session.c`, `pagelist.c`, `pnfs.h` under
`fs/nfs/`. In each case what is reachable is the pure-logic seam inside a
file that is otherwise VFS- and RPC-entangled -- see
[xfstests-vs-pynfs.md](xfstests-vs-pynfs.md) and
["fs/nfs is not wholly untestable"](#fsnfs-is-not-wholly-untestable) for
where that boundary sits.

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
in this file: in `fs/nfs`, protocol operations are indirect calls, so
"needs a server" is almost never the real obstacle.

`nfs_find_actor()`: a false positive there hands back the wrong inode
entirely, and the filehandle check is what catches two different files that
happen to share a fileid across filesystems. The callback is pure, so it can
be driven directly without involving the inode cache.

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

A zero-length layout range does **not** intersect itself, but **does**
intersect any range that strictly straddles its offset, because the
predicate is `start2 < end1 && start1 < end2`. The test asserts that rather
than the intuitive "empty intersects nothing."

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
`nfs_set_cache_invalid()` do not need a working page cache: both only read
`mapping->nrpages` as a count, so a zeroed `struct address_space` with
that one field set is enough. `nfs_have_delegated_attributes()` is
reachable the same way: it dispatches through
`NFS_PROTO(inode)->have_delegation` — a function pointer, so a three-line
stub replaces the whole delegation subsystem.

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

## The xfstests ports

The `kunit/xfstests/` tree holds ports of **120 xfstests generic cases**,
each a KUnit suite named after its original (`xfstests/generic/001` ...),
each running against a real NFS mount served by knfsd inside the same UML
kernel. The deployment lives in `kunit/xfstests/nfs_fixture.{c,h}`: tmpfs
on `/export` (size settable per suite for the ENOSPC family), knfsd
v4-only on 127.0.0.1:2049, the real client mounted as NFSv4.2 on
`/mnt/nfs`. mountd's three caches are fed directly; nfsdfs is mounted
because `create_client()` needs it; grace is ended the `v4_end_grace` way.
Bring-up is refcounted per suite, so every full run also exercises ~60
consecutive nfsd restart and mount/unmount cycles.

### The per-test-case thread

KUnit runs each test case's body in its own fresh kthread, and that kthread
does not share `suite_init()`'s `fs_struct`/root. On current mainline the
case thread's root never saw the `mkdir`/`mount` calls that bring-up made,
so the first path lookup under the mount point fails with `ENOENT` before
reaching any NFS code at all -- a failure that looks like a broken mount and
is not.

`xfstests_nfs_case_init()` (`kunit/xfstests/nfs_fixture.c`) rebinds each
fresh case thread to the bring-up thread's root before the test body runs.
Every xfstests suite needs it wired in as `.init`; `run-nfs-kunit.sh`
adds that automatically, so a new port gets it without doing anything. The
exception is `nfs_fixture` itself, which defines no suite.

Ported:

```
001 002 005 006 007 011 013 014 020 023 028 029 030 035 037 069 070 074
075 080 084 086 087 088 089 100 103 109 123 124 125 126 129 130 131 132
133 135 141 169 184 193 213 214 215 221 228 236 245 246 247 248 249 257
258 285 286 306 308 309 310 313 314 337 340 344 346 354 355 360 364 377
378 391 393 394 401 406 412 423 430 431 432 433 434 438 443 448 450 453
454 464 471 486 490 523 525 528 532 533 568 609 611 615 618 637 638 639
647 676 680 706 707 708 728 729 736 749 755 763
```

120 of upstream's 798 `generic/` cases. Every one of the other 678 is
accounted for in
[xfstests-ports-not-done.md](xfstests-ports-not-done.md), which names,
per test, what cannot be reproduced -- generated from the test sources so
it stays checkable.

Two rules bound the set. A case upstream reports `[not run]` on an
NFSv4.2 mount is not ported, since there is no upstream result to mirror;
measured with `scripts/00-run-xfstests-on-vm-and-docker.sh` against knfsd
in docker at `vers=4.2`. And a case whose subject is a userspace program
rather than the filesystem is not ported either -- generic/010 drives
ndbm through `src/dbtest`, generic/241 is dbench, generic/759 is fsx on
hugepages.

Nothing in the ported set covers crash consistency: dm-flakey needs a block
device, and the fixture exports tmpfs with client and server in one kernel,
so writes cannot be dropped and replayed. That is the largest single gap.

Each port is meant to perform upstream's operations and assert upstream's
outcome, at reduced scale where the original's magnitudes do not fit an
in-kernel tmpfs export, and single-threaded where the original forks. Where
that reduction loses the point of the test, the port says so in its header;
where upstream keeps an NFS-specific golden image (`035.out.nfs`), the port
follows it rather than the default one.

The families, by what they exercise:

- **Namespace semantics**: the 023 rename matrix, 028 path resolution
  across renames, 035 sillyrename-on-rename-over, 089 mtab link/rename
  churn, 109, 245, 309, 360 long symlink targets, 453 filenames that are
  byte strings rather than text, 736 readdir while every entry is being
  renamed, 707 a directory moved while it grows.
- **Data integrity**: 001 chain copier, 014 truncfile, 075 mini-fsx with a
  shadow model, 029/030 mapped writes, 069 O_APPEND, 074 fstest with
  three writer kthreads, 100 a copied tree, 124 positional patterns, 129,
  132, 169, 213 ALLOCATE boundaries, 214 writes into preallocated ranges,
  286 seek-driven sparse copy, 308 1TB offsets, 525 the top of the 64-bit
  offset range, 406 one large direct write, 639 a write beside uncached
  data.
- **Direct I/O**: 130 the buffered/direct battery, 135 the three write
  paths, 412 a truncate into a hole between them, 450 reads at and past
  EOF, 609 O_DIRECT with O_DSYNC, 125 direct reads after a truncate, 355
  suid stripped on a direct write.
- **Faults and mappings**: 246 writev from a mapping, 248 pwrite from the
  same page, 443 writev faulting on its own iovecs, 638 an overlapping
  pwritev, 647/729 reads and writes whose buffer is the file's own
  mapping, 708 a direct write from a half-faulted mapping, 749 the tail of
  the last page, 080/215 mapped writes and timestamps, 141 a mapping at a
  non-zero offset.
- **Timestamps**: 221, 236, 313, 258 pre-epoch times, 528 statx btime, 728
  ctime after an xattr operation, 755 ctime after unlinking a link.
- **Xattrs** -- RFC 8276 works end to end here: 020, 037, 070
  model-checked storm, 337 listxattr completeness, 377 its buffer sizes,
  454 keys that are byte strings, 486 XATTR_REPLACE, 523 a slash in a
  name, 533 the smoke test, 611 an empty value, 618 two mid-sized values,
  103 large values at ENOSPC.
- **Permissions**, via in-kernel credential switching with dropped
  capabilities: 087, 088, 123, 126, 193, 314 SGID inheritance, 378 a hard
  link's shared mode, 680 the Dirty Pipe CVE.
- **Directories and readdir cookies**: 257 cookie stability, 471
  rewinddir, 637 a directory modified mid-walk, 676 seekdir to valid and
  invalid offsets, 310 read(2) racing readdir, 401 d_type for every entry
  type, 011/013 the directory-stress pair.
- **copy_file_range as NFSv4.2 COPY**: 430 into new files, 431 one byte at
  a time, 432/433 rearranging an existing file, 434 what it must refuse;
  249 is the sendfile/splice equivalent.
- **Concurrency**, each with one kthread beside the test thread: 084 a
  link storm against a vanishing target, 133 a reader and a writer, 247 a
  direct overwriter against a mapped writer, 340/344/346/354 the holetest
  family, 364 direct writes and fsync on one fd, 391 interleaved direct
  reads, 438 fallocate-and-mmap under a fsync loop, 464 mixed writes and
  syncs, 615 st_blocks during writeback.
- Plus POSIX locks as NFSv4 LOCK state (131), SEEK RPCs (285/286, 490,
  706, 448), RLIMIT_FSIZE (228 fallocate, 394 truncate), symlink ELOOP
  limits (005), mknod (184), statx across object types (423, 532), and
  the read-only mount rules (306).

NFS-specific semantics the porting surfaced and pinned, each found as a
failing "wrong" expectation and verified before being encoded:

- A same-size truncate is optimised away by the client -- no SETATTR, no
  ctime/mtime update -- diverging from local filesystems (313).
- Renaming over an open target sillyrenames it: nlink stays 1 and a .nfs
  entry appears until the last close (035); removal storms can leave
  transient .nfs entries, hence the fixture's settled rmdir (011/013).
- In-kernel opens lack force_o_largefile(): without O_LARGEFILE the 2GiB
  MAX_NON_LFS limit applies (308).
- xattr gets are served from the client's xattr cache, so only a
  server-side check through the export directory proves the SETXATTR wire
  value (020/037/070).
- A write's dirty range is rounded back up to the page boundary by
  `nfs_update_folio()`, so a "drop the last byte" mutation is absorbed
  entirely for page-aligned writes and only the unaligned ports catch it.
- **A delegation hides a SETATTR from the server.** With a write
  delegation on the file, `nfs_setattr()` applies `ATTR_MTIME_SET`/
  `ATTR_ATIME_SET` locally through `nfs_set_timestamps_to_ts()` and clears
  the bits, so no SETATTR is sent: the client reports the new timestamps
  and the server still holds the old ones. Found while porting 258, whose
  server-side check failed until the file under test was one the client
  had never opened. The port now covers both sides.
- **A negative SEEK_HOLE/SEEK_DATA offset comes back as ENXIO, not
  EINVAL** (448). `nfs4_file_llseek()` hands both whences straight to
  `nfs42_proc_llseek()`, so the offset is encoded as an unsigned offset4
  and goes to the server, which answers NFS4ERR_NXIO because it is past
  EOF. ENXIO is what seek_sanity_test's own `do_lseek()` requires, so this
  is upstream's expectation reached by an unusual route.
- **suid and sgid are stripped by the server, and the client notices**
  (355): the WRITE carries no mode, knfsd applies the POSIX rule, and a
  forced revalidation on the client agrees with the server's own file.
- **seek_sanity_test's unwritten-extent cases are vacuous on this stack**
  (436, 445): the program only sets `unwritten_extents` when a
  fallocate'd range still reads as a hole, and ALLOCATE against the tmpfs
  export allocates real zeroed pages.
- **The client only learned to report statx btime after v6.12.57** (528):
  `fs/nfs/inode.c` has no `STATX_BTIME` on that release and five
  references to it on v6.18.52, v7.2.6 and mainline. Upstream's answer is
  `_require_btime`, which notruns. The port does not simply skip when the
  bit is missing, because that would also hide a regression on a kernel
  that does support it: it tells the two apart at compile time with
  `NFS_ATTR_FATTR_BTIME` (include/linux/nfs_xdr.h), which is absent on
  v6.12.57 and present on the other three refs -- the same boundary. Where
  the client knows about btime, not reporting it fails; where it does not,
  the case skips. CI's v6.12.57 leg found this, not the VM.

Two in-kernel mechanics the newer ports needed, both worth knowing before
writing another one:

- **O_DIRECT cannot be driven with `kernel_read()`/`kernel_write()`.**
  Their ITER_KVEC reaches `iov_iter_get_pages_alloc2()` (via
  `nfs_direct_{read,write}_schedule_iovec()`), which handles user-backed,
  bvec, folioq and xarray iterators and returns **-EFAULT** for a kvec.
  An ITER_BVEC over the same kmalloc'd buffer works, and that is what
  `xfs_direct_read()`/`xfs_direct_write()` build. For a buffer that is a
  user address inside a `kunit_vm_mmap()` mapping there is
  `xfs_user_rw()`, which is pread/pwrite's iterator with the direct flag
  as a parameter.
- **`kernel_write()` refuses any file whose fops wire up both `->write`
  and `->write_iter`** ("implies very convoluted semantics",
  fs/read_write.c) -- which is exactly what `null_fops` and a directory's
  `generic_read_dir` do. Writing to a device node (184, 306) or reading a
  directory (310) therefore goes through `vfs_write()`/`vfs_read()` with a
  user address, which is the syscall's own path.
- **A worker kthread cannot use KUnit assertions**: they unwind through
  the test thread's try_catch. The concurrency ports (074, 084, 133, 247,
  310, 340, 344, 346, 354, 364, 391, 438, 464, 615, 707) record the
  worker's first error in a struct and let the test thread assert on it.
  A worker that has to touch a `kunit_vm_mmap()` mapping calls
  `kthread_use_mm()` with the test thread's mm -- which only exists after
  the first `kunit_vm_mmap()` call, so take the pointer after it, not
  before.
- **`iterate_dir()` is one getdents(2), not a whole directory.** It
  returns a batch; a port that reads a directory has to call it until a
  pass adds nothing, or it will silently check only the first ~17 entries.
- **An in-kernel unlink of a file the test itself wrote can sillyrename
  it.** `filp_close()` from a kernel thread defers the final `fput` to a
  workqueue, and NFS turns an unlink whose inode still has a live struct
  file into a RENAME to `.nfsXXXX`; the name then disappears on its own
  when the fput lands. A port that unlinks a file and then cares what the
  directory contains has to call `xfs_settle_fput()` first. Found as an
  intermittent generic/637 failure on CI -- a cookie that had pointed at
  a transient `.nfs...` entry later returned a different name -- after
  twenty clean runs on the VM.

One thing is measured but not explained, and the port that hit it says so
rather than working around it quietly: after the client mount is
remounted read-only, **the first open of a device node whose dentry is no
longer cached fails with -EIO**, repeatedly, while a plain `kern_path()`
or any getattr first makes the same open succeed (306). fs/namei.c's
`atomic_open()` turns "`->atomic_open()` returned 0 without opening" into
-EIO, which fits, but that path also WARNs and no warning appears in the
log.

Twice during a full run the UML kernel has panicked in a way that is not
attributed: once in `nfs_delegation_find_inode()` on the NFSv4 callback
thread handling a CB_RECALL, once in `refill_objects()` on the nfsd
thread, both "Segfault with no mm" with a small bad pointer. Neither
reproduced on a re-run, and a run with `slub_debug=FZPU` reported no slab
corruption. It is recorded here because the ports' server-side checks
(reading the export directly while the client holds a delegation) make
the client's CB_RECALL path run far more often than it used to.

029 and 030 both need `vm_mmap()`, which normally requires `current->mm` --
absent in the kernel thread a KUnit case runs in. That is not a blocker:
KUnit ships `kunit_vm_mmap()` (`lib/kunit/user_alloc.c`), which allocates an
mm, runs `arch_pick_mmap_layout()` on it, attaches it with
`kthread_use_mm()`, and tracks the mapping as a test resource -- `mm_alloc()`
is already `EXPORT_SYMBOL_IF_KUNIT` for the purpose, and the helper is built
into `lib/kunit` unconditionally. So **mmap is available to every port**,
and 029 and 030 are both in. Writes into a mapping go through
`copy_to_user()`, which is the correct way to touch user addresses with a
borrowed mm (a bare dereference happens to work on UML but not under SMAP or
PAN).

**030** additionally drives `mremap` around its truncates, and `mremap` is
unreachable from a KUnit case: it exists only as a syscall entry point
(`SYSCALL_DEFINE5(mremap, ...)`) with static helpers, and `nm` on
`.kunit/vmlinux` shows `__do_sys_mremap`, `__se_sys_mremap` and
`sys_mremap` all as local symbols (`t`, not `T`). There is no `vm_mmap()`
equivalent.

That does not matter, because 030's `mremap` calls are no-ops. `mremap`
rounds both lengths up to a page, and 030's file is 5017k -- 1254.25 pages.
`PAGE_ALIGN(5017k)` and `PAGE_ALIGN(5020k)` are both 5020k, so every
`mremap -m 5020k` / `mremap 5017k` takes the `old_len == new_len` path and
returns the same address without touching a VMA. What they resize is
xfs_io's own record of the mapping length, which is what lets its next
`mwrite` clear its own bounds check; a 5017k mapping already covers
everything 030 writes. `kunit/xfstests/generic/030.c` asserts that rounding
directly, so if the premise stops holding the test says so rather than
silently drifting.

030 adds a second layout over 029's code path: unaligned mapped writes
inside the last page of a ~5 MB file, versus 029's page-multiple 5 KB ones.
Two mutations run against both give a split answer:

| mutation | 029 | 030 |
|---|---|---|
| drop `truncate_pagecache()` in `nfs_vmtruncate()` (inode.c:811) | catches | catches |
| drop `nfs_folio_length()`'s partial-last-folio clamp (internal.h) | catches | **misses** |

The clamp mutation is caught by 029 because its third case is 5121 bytes, and
is invisible to 030 -- including to the mid-test check described next.
**030 is not a strictly stronger 029**; the two catch different mutations
and neither subsumes the other.

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

### Known gaps from upstream, found by audit

An audit compared all 120 ports against their `xfstests/tests/generic/NNN`
originals, checking not just whether a port's header discloses a scale
reduction but whether the reduced or altered version can still fail the way
the original would. Two ports have a gap; the rest -- including every
concurrency port that races a real second kthread (028, 084, 133, 247, 340,
344, 346, 354, 391, 707) -- hold up: the reduced scale still exercises the
same code path and can still fail the same way the original does.

**Scope reduction, understated but not structurally broken:**

- **286**: keeps only upstream's test01 (pure holes+data); drops test02-04
  (falloc'd unwritten-extent layouts). Likely justified -- ALLOCATE against
  the tmpfs export produces real zeroed pages, not unwritten extents, the
  same reasoning already used for 436/445 -- but the header doesn't say so.
- **749**: ports 2 of upstream's 6 file-length/block-size parameterizations.
  The dropped 4 are how upstream reaches its target bug
  (`folio_map_range()`, gated on `block_size > PAGE_SIZE`); tmpfs/NFS has no
  block size distinct from the page size, so that scenario can't be
  reproduced on this fixture at any scale. The port's kept case is a real,
  correctly-checked property, but a different and weaker claim than what
  generic/749 was written to catch, and the header doesn't say so.

### A confirmed host-signal livelock on v6.12.57 (upstream bug, backported here)

A full run sometimes never finishes. Two distinct symptoms were observed
under gdb on the VM, both on the `v6.12.57` pin:

- A transient stall of tens of seconds -- the UML tracer process asleep in
  `sigsuspend()`, its ptraced child stuck in `ptrace_stop`, waiting on each
  other -- that resolves on its own and the run completes normally. This
  is the likely explanation for hang reports that turned out to be
  transient.
- A **permanent** livelock: the UML process pinned at ~99% CPU with no
  further KTAP output, reproduced and confirmed with `gdb -p <pid> -batch -ex
  bt` taken several seconds apart, landing at the exact same PC every time --
  not just the same function, the identical instruction. The frame was inside
  `hard_handler()` / `to_irq_stack()` in `arch/um/os-Linux/signal.c` and
  `arch/um/kernel/irq.c`.

`to_irq_stack()`/`from_irq_stack()` is UML's mechanism for copying the
current task's `thread_info` onto a separate signal (IRQ) stack, guarded by a
lock-free `pending_mask` retry loop. Its own comment in `irq.c` on the
`v6.12.57` pin admits the danger: "What happens when two signals race each
other? UML doesn't block signals with sigprocmask, SA_DEFER, or sa_mask, so a
second signal could arrive while a previous one is still setting up the
thread_info." Under the right timing -- many suites in one run means many
timer ticks and ptrace child-stop signals landing close together -- that race
can leave `hard_handler()`'s `do { ... } while (pending)` loop spinning
forever, unable to make forward progress.

This mechanism does not exist on later kernels: diffing
`arch/um/os-Linux/signal.c` and `arch/um/kernel/irq.c` between `v6.12.57` and
`v6.18` shows `to_irq_stack()`/`from_irq_stack()` and the `pending_mask`
dance removed outright, replaced by direct dispatch to `handlers[sig]`, with
`SIGCHLD` also promoted to a first-class signal in that table (previously
absent from it). Upstream commit
[`2f681ba4b352`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/commit/?id=2f681ba4b352cdd5658ed2a96062375a12839755)
("um: move thread info into task", Benjamin Berg, 2024-11-12) is that
rewrite: it selects
`THREAD_INFO_IN_TASK` for UML, using the existing `cpu_tasks[]` tracking
instead of a per-signal-stack copy of `thread_info`, and its own commit
message says so directly -- "Also remove the signal handler code that
copies the thread information into the IRQ stack. It is obsolete now, which
also means that the mentioned race condition cannot happen anymore." It
landed on mainline after the `v6.12.57` point release and reached `v6.18`;
it was not backported to the `v6.12.x` stable branch. That accounts for the
CI matrix result: **only the `v6.12.57` leg hangs; `v6.18.52`, `v7.2.6`, and
`master` do not.**

Consistent with the above: this is a livelock rather than a leak, since it
can strike after any suite that generates enough signal traffic, not
specifically after xfstests:

- It stalls somewhere in the suites that follow the xfstests block, observed
  after `sunrpc-rtt-init`, after `sunrpc-addr-uaddr`, and after
  `sunrpc-rtt-ntimeo` on different runs. Every suite passes in isolation.
- It is not caused by any of this repo's kunit ports, and not memory
  pressure (`--kernel_args mem=2G` does not fix it) -- both consistent with
  the bug living entirely in UML's own host-signal plumbing, outside
  anything under test here.
- It is intermittent, matching a timing race rather than a deterministic
  trigger.

#### The backport

Fixing this means patching the vendored kernel tree, since the race is in
the pinned kernel's own `arch/um` code.
`patches/um-thread-info-in-task-v6.12.57.patch` is that backport of
`2f681ba4b352` to `v6.12.57`. It is not a straight cherry-pick: three files
have diverged from the commit's mainline base through unrelated stable
backports (an `aux_fp_regs` field in `thread_info`, a `highmem` argument on
`setup_physmem()`/`mem_total_pages()`, an already-present
`HAVE_ARCH_TRACEHOOK` select), so those conflicts are resolved by hand. The
patch header records each one.

`.github/workflows/kunit.yml` applies it in the `kunit-v6-12-57-patched`
job, which fetches `v6.12.57`, applies the patch, and runs the full
unfiltered suite to completion. The `v6.12.57` leg of the `kunit` matrix
deliberately stays **unpatched**, so the two jobs together show both the bug
and the fix. Both jobs set `timeout-minutes: 30`, which on the unpatched leg
is the mitigation for a livelocked run: it fails fast and visibly rather
than consuming GitHub Actions' default 360-minute budget.

Applying it by hand:

```sh
scripts/fetch-sources.sh linux
git -C linux apply "$PWD/patches/um-thread-info-in-task-v6.12.57.patch"
```

`git -C linux apply` resolves a relative patch path against `linux/`, not
the directory it was invoked from, so the path must be absolute.

## Coverage

`COVERAGE=1 scripts/kunit/run-nfs-kunit.sh` (see
[kunit-nfs.md#coverage](kunit-nfs.md#coverage)) turns on UML's own gcov path
(`CONFIG_GCOV`, `arch/um/Kconfig.debug`) rather than the debugfs-based
`CONFIG_GCOV_KERNEL` other architectures use, since UML is an ordinary
process and can write `.gcda` straight into the build dir. Getting a real
report out of that needed two workarounds, both applied by the script and
confirmed on this repo's own `kunit-coverage` CI job.

### UML's linker scripts drop gcov's exit destructor

A `COVERAGE=1` run built and ran cleanly but produced zero `.gcda` files —
confirmed directly: 1130 `.gcno` (written at compile time) against 0 `.gcda`
(written at process exit) after a full run. gcc emits each translation
unit's gcov flush routine (`__gcov_exit`) into a *prioritized*
`.fini_array.NNNNN` section, not the bare `.fini_array`. UML's linker
scripts (`arch/um/include/asm/common.lds.S`, `arch/um/kernel/dyn.lds.S`)
only collect `*(.fini_array)` into the final `.fini_array` output section —
unlike `.init_array`, where both `*(.init_array.*)` and `*(.init_array)`
are already collected. The prioritized destructor entries are silently
dropped, so `__gcov_exit` never runs at process exit and no coverage data
is ever written, gcov-instrumented or not.

This is not new or specific to this repo: it is the same failure as the
still-unmerged
[RFC posted May 2026](https://ratatoskr.run/linux-um/2026/05/16442684/t),
which traces back to a 2021 patch from Johannes Berg
([lore](https://lkml.iu.edu/hypermail/linux/kernel/2103.1/06608.html)) that
also never landed. No upstream fix exists as of this writing.
`run-nfs-kunit.sh` works around it directly, grep-guarded like the rest of
its tree edits: under `COVERAGE=1`, it adds `*(.fini_array.*)` ahead of
`*(.fini_array)` in both linker scripts before the build.

### `geninfo`'s line-mismatch error on syscall wrappers

With `.gcda` files actually present, `lcov -c` still failed outright on
`master`:

```
geninfo: ERROR: mismatched end line for __do_sys_socketcall at net/socket.c:3202: 3202 -> 3319
```

`geninfo` misattributes the end line of some syscall-wrapper macros (this
one expands `SYSCALL_DEFINE2`) across gcc/lcov version combinations, and
without `--ignore-errors mismatch` that is a hard error rather than a
warning — the message names its own fix. `run-nfs-kunit.sh` passes
`--ignore-errors mismatch` to `lcov -c` under `COVERAGE=1`.

### A note on green results and kernel logs

**A green KUnit result says nothing about what the kernel logged underneath
it.** Nothing in the runner fails a suite for WARNing, and the default
(non-raw) `kunit.py` output does not show kernel log lines at all. A case
can pass while emitting hundreds of WARNs -- a thread calling
`sync_filesystem()` without holding `s_umount` trips
`WARN_ON(!rwsem_is_locked(&sb->s_umount))` three times per call and still
reports PASSED. Run with `--raw_output` when a port does anything the VFS
expects a syscall wrapper to have set up.

## Why the unit tests are not ports

The unit suites are not miniature xfstests cases; they test the layer
beneath one. `generic/007` checks that `open`/`unlink`/`stat` return
sensible errnos end to end, and `fs/nfs_common/common.c` is the pure lookup
deciding what those errnos are. The `generic/008`/`009` page-boundary cases
have their analogue in XDR's 4-byte alignment rule. That is how every file
in `kunit/*.c` was chosen: find the pure decision underneath a system-level
concern and pin it directly.

A case whose subject *is* the system -- "does `relatime` suppress this atime
update", journal replay after a crash, anything in the `shutdown` or
`fiemap` groups -- has no such layer to drop to. Those either get a real
mount (the `kunit/xfstests/` ports above) or they do not get tested here.

## fs/nfs is not wholly untestable

`fs/nfs` is entangled with the VFS, which makes most of it unreachable
from KUnit -- but that is not a blanket statement about the whole
directory.

Of the 64 `.c` files in `fs/nfs`, several reference no inode, dentry, page
or file at all — among them `fs_context.c` (mount option parsing, 1684
lines), `nfs42xdr.c` (1674), `callback_xdr.c` (1141), `nfs4session.c` (657)
and `mount_clnt.c` (539).

`kunit/nfs4session_test.c` exists to settle the point concretely. NFSv4.1
session slot tables are bitmap allocation plus a control loop, need no I/O
and no server, and their whole API is exported through
`fs/nfs/nfs4session.h`. They also sit squarely inside "NFSv4 state," which
is otherwise a plausible category to write off as untestable.

What remains genuinely out of reach for KUnit is narrower than first
stated: anything requiring a mounted filesystem, a socket, an RPC round
trip, or a live server. `fs_context.c`'s parsers are reachable too, though
they are `static` and would need `VISIBLE_IF_KUNIT` plus a constructed
`struct fs_context`.

## The UBSAN artefact

The stock `.kunitconfig` sets `CONFIG_UBSAN=y`, and under the UML build
UBSAN reports a misaligned access unrelated to any of this code:

```
UBSAN: misaligned-access in ../kernel/exit.c:774:2
member access within misaligned address ... for type 'struct task_struct'
which requires 64 byte alignment
```

KUnit attributes whatever lands in the log to whichever test happens to be
running, so this surfaces as a failed case — and which case it lands on
shifts with binary layout, so it moves between runs. Suppress it with:

```sh
kunit.py run --kunitconfig=net/sunrpc/.kunitconfig --kconfig_add CONFIG_UBSAN=n
```

A red result from this artefact is distinguishable from a real one: check
the raw output for an `EXPECTATION FAILED` line. A UBSAN stack trace with
no such line is this artefact. In particular `64-fold("012345")` and
`Encrypt empty plaintext with aes128-cts-hmac-sha256-128` are not
pre-existing upstream failures at `v6.12.57` -- upstream's
`gss_krb5_test.c` passes in full.

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
