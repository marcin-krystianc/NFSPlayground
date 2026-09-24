# How closely the xfstests ports follow upstream

[kunit-nfs-reference.md](kunit-nfs-reference.md#the-xfstests-ports) says
each port is meant to perform upstream's operations and assert upstream's
outcome. This file records how far the 120 ports in
[kunit/xfstests/generic/](../kunit/xfstests/generic/) meet that. Each port
was compared with the upstream test script, its golden output and the
`src/` helper it runs, then changed to follow them. Every remaining
difference is stated in the port's header.

Compared against the `xfstests` submodule at `56c410ad` ("fstests:
formalize and fix disabling the RT subvolume").

- 3 ports run a substitute for upstream's program: 013 and 070 instead of
  fsstress, 075 instead of fsx.
- 2 ports leave out part of upstream: 306 (the bind mount) and 354 (the
  `-F` runs).
- 17 ports run upstream's steps at a smaller size, count or duration.
- The rest follow upstream step for step. Several add checks that
  upstream does not make; the headers mark them "not in upstream".

Status values in the table at the end:

- **Follows**: upstream's steps and outcome. Stated substitutions of
  mechanism are allowed (for example, a server-side read in place of a
  mount cycle).
- **Reduced**: upstream's steps at a smaller scale, stated.
- **Partial**: some of upstream's steps are not ported, stated.
- **Substitute**: a different program, stated.

## Substitutes

| Test | Upstream | Port |
|------|----------|------|
| 013 | fsstress. | A mini-fsstress: 10 op types, 2,000 ops. |
| 070 | fsstress `-p 1 -n 10000`, with `attr_set` and `attr_remove` weighted 100 on top of the default operation mix. | A model-checked xattr storm: 30 files, 10 names each, 1,500 set/get/remove/list ops. No other fsstress ops. |
| 075 | fsx `-N 1000` and `-N 10000 -l 10MB`, time-based seeds. Two more runs with `-x` are skipped on NFS, because `xfs_io resvsp` fails there. | One 5,000-op run, fixed seed, 256 KB cap. Only write, truncate, punch hole and read. |

Following upstream here means porting fsstress (5,742 lines) or fsx
(3,685 lines) into the kernel.

## Upstream steps not ported

- 306: the bind-mounted file. Mounting inside the shared fixture would
  change what the other suites see.
- 354: the `holetest -F` runs. They need one address space per marker;
  the markers here are kthreads sharing one mm.

## Reduced scale or duration

| Test | Upstream | Port |
|------|----------|------|
| 028 | Time-bounded rename race. | 2,000 path resolutions. |
| 074 | `uname -a` picks 10/5/3 loops/files/children on SMP. The UML kernel has `CONFIG_SMP=y`. | 2/3/3, upstream's non-SMP set. 10/5/3 needs 450 MiB of export. |
| 080, 215 | `sleep 2`, times compared in whole seconds. | 20 ms, times compared to the nanosecond. |
| 084 | One link storm per CPU, until killed. | Two kthreads, bounded rounds. |
| 125 | Direct reads for 60 s. | 10 s. |
| 129 | Set 1: 100,000 iterations. | 30,000. 100,000 leaves 800 MiB in the file. |
| 133 | 512 MiB per round. | 64 MiB. 256 MiB took 29 s on the VM. |
| 213 | 1 GiB scenarios. | Scaled to 8 MiB. |
| 247 | 512 MiB in 1 MiB steps. | 4 MiB in 64 KiB steps. |
| 249 | 32 MiB. | 4 MiB. |
| 310 | 30 s per program. | 5 s. |
| 364 | Until a 10 s timeout. | 5 s. |
| 391 | 1,024 extents, readers on a barrier. | 64 extents, readers run freely. |
| 406 | One 258 MiB direct write. | One 2 MiB direct write. |
| 464 | 10 rounds of 5 s. | 2 rounds of 3 s. |
| 707 | 500 parents, 500 files, 100 loops. | 20, 60, 3. |

## Other stated substitutions

- A mount cycle becomes a read of the server's copy through the tmpfs
  export: 029, 030, 169, 393, 525, 533, 618.
- 124: NFSv4.2 ALLOCATE stands in for `XFS_IOC_RESVSP`.
- 126: `open_exec()` stands in for exec.
- 377: case 2 names a missing file instead of `""`. The syscall's ENOENT
  for `""` comes from `getname()`; `kern_path("")` resolves to the cwd.
- 401: exact `d_type` is asserted, which is upstream's result when its
  `_supports_filetype` probe passes. The probe is not run.
- 708: a positional pattern instead of 0xcd.
- readdir(3) is getdents64 with glibc's buffer size, from
  `xfs_libc_dirbuf()`: 006, 310, 471, 676, 736. glibc 2.39 used 32768
  bytes for a directory with `st_blksize` 4096, and 1048576 on an NFS
  mount whose `st_blksize` was 1048576.

## Run time

Several ports now run at upstream scale. On the VM, 438 (256 KiB, one
byte at a time) takes about 50 s. The whole suite's run time has not been
measured against kunit.py's default `--timeout` of 300 s.

## Every port

| Test | Status | Notes |
|------|--------|-------|
| 001 | Follows | PRNG fill and non-bitwise awk schedule stated. |
| 002 | Follows | |
| 005 | Follows | Adds the `MAXSYMLINKS` boundary; opens instead of `touch`. |
| 006 | Follows | Both runs; `find`'s count is a getdents walk. |
| 007 | Follows | Seed 1, 100,000 iterations; totals equal `007.out`. |
| 011 | Follows | `-f 1000`, three tests; processes are kthreads. |
| 013 | Substitute | Mini-fsstress. |
| 014 | Follows | 10,000 rounds over 256 MiB; `ftruncate` on the open fd. |
| 020 | Follows | Full sequence, `max_attrs` 1000 and 1024-byte values for nfs. |
| 023 | Follows | Full 5x5 matrix, same- and cross-directory; two extra cases. |
| 028 | Reduced | See above. |
| 029 | Follows | |
| 030 | Follows | All three scenarios. |
| 035 | Follows | Follows `035.out.nfs`. |
| 037 | Follows | |
| 069 | Follows | Six writers at upstream's counts. |
| 070 | Substitute | See above. |
| 074 | Reduced | See above. |
| 075 | Substitute | See above. |
| 080 | Reduced | See above. |
| 084 | Reduced | See above. |
| 086 | Follows | `drop_caches` is `invalidate_inode_pages2` on the inode. |
| 087 | Follows | |
| 088 | Follows | |
| 089 | Follows | t_mtab's lock, copy and rename, 3 x 50 then 10,000, three directory sizes. |
| 100 | Follows | Upstream's tree size; a copy instead of tar. |
| 103 | Follows | The `attrval` file is not created. |
| 109 | Follows | Each `dirN` is removed after use; upstream keeps all 20. |
| 123 | Follows | |
| 124 | Follows | 100 rounds per pass, one 1 MiB write and read. |
| 125 | Reduced | See above. |
| 126 | Follows | Upstream's 18 rows. |
| 129 | Reduced | See above. |
| 130 | Follows | |
| 131 | Follows | locktest's 221 Linux steps. |
| 132 | Follows | All 13 stages. |
| 133 | Reduced | See above. |
| 135 | Follows | |
| 141 | Follows | |
| 169 | Follows | |
| 184 | Follows | Adds a fifo. |
| 193 | Follows | |
| 213 | Reduced | See above. |
| 214 | Follows | |
| 215 | Reduced | See above. |
| 221 | Follows | Path-based utimes instead of `futimens`. |
| 228 | Follows | 100 MiB limit; 101 MiB fails, 50 MiB succeeds. |
| 236 | Follows | |
| 245 | Follows | Cross-directory rename; EEXIST or ENOTEMPTY. |
| 246 | Follows | |
| 247 | Reduced | See above. |
| 248 | Follows | |
| 249 | Reduced | See above. |
| 257 | Follows | `t_dir_offset2` on 168 entries. |
| 258 | Follows | Split into two cases. |
| 285 | Follows | `seek_sanity_test` sub-tests 1 to 12, upstream's default range. |
| 286 | Follows | |
| 306 | Partial | See above. |
| 308 | Follows | |
| 309 | Follows | |
| 310 | Reduced | See above. |
| 313 | Follows | |
| 314 | Follows | |
| 337 | Follows | Names compared as a set. |
| 340 | Follows | holetest's three sizings. |
| 344 | Follows | holetest's three sizings. |
| 346 | Follows | holetest's three sizings. |
| 354 | Partial | See above. |
| 355 | Follows | |
| 360 | Follows | |
| 364 | Reduced | See above. |
| 377 | Follows | Sizes 1, 9, 11, 500. |
| 378 | Follows | Adds the reverse direction. |
| 391 | Reduced | See above. |
| 393 | Follows | All four scenarios. |
| 394 | Follows | |
| 401 | Follows | Includes `.` and `..` as DT_DIR. |
| 406 | Reduced | See above. |
| 412 | Follows | |
| 423 | Follows | The socket is made with `mknod(S_IFSOCK)`. |
| 430 | Follows | |
| 431 | Follows | |
| 432 | Follows | |
| 433 | Follows | |
| 434 | Follows | Device and fifo opened `O_RDWR`; upstream adds `O_APPEND`. |
| 438 | Follows | 256 KiB; the fsync loop reopens the file each time. |
| 443 | Follows | |
| 448 | Follows | |
| 450 | Follows | Far offset 32768 instead of `bsize * 100`. |
| 453 | Follows | All of upstream's names. |
| 454 | Follows | All 61 keys. |
| 464 | Reduced | See above. |
| 471 | Follows | 10,000 files. |
| 486 | Follows | 2048 bytes instead of `st_blksize * 3/4` (stated). |
| 490 | Follows | Block size 4096 rather than probed. |
| 523 | Follows | |
| 525 | Follows | |
| 528 | Follows | |
| 532 | Follows | `chattr +i`/`+a` omitted; they fail on NFS and upstream skips them. |
| 533 | Follows | Full sequence. |
| 568 | Follows | |
| 609 | Follows | `O_DIRECT` and `O_SYNC`, 16 writes of 4 KiB. |
| 611 | Follows | |
| 615 | Follows | 2,000 opens per mode. |
| 618 | Follows | |
| 637 | Follows | `t_dir_offset2` with `+0`, then `-10` to `-100`. |
| 638 | Follows | |
| 639 | Follows | Cache drop is `invalidate_inode_pages2` (stated). |
| 647 | Follows | |
| 676 | Follows | 4,000 files, both walks, `$RANDOM` seed. |
| 680 | Follows | |
| 706 | Follows | |
| 707 | Reduced | See above. |
| 708 | Follows | 2 MiB. |
| 728 | Follows | |
| 729 | Follows | All six cases. |
| 736 | Follows | 5,000 files, glibc-sized batches. |
| 749 | Follows | Six parameter sets; deviations stated. |
| 755 | Follows | |
| 763 | Follows | |
