# read() returning EBUSY on NFS: an invalidation failure reaching userspace

A buffered `read()` on an NFS mount fails with `EBUSY` while the same file is
being written concurrently. In xfstests it shows up as an intermittent
`generic/095` failure. The cause is in the NFS client: a transient failure to
invalidate the page cache is returned to `read(2)` instead of being retried.
There is no patch yet. This file is the analysis and the reproducer.

Code references are upstream master `4982d3552` (v7.3-rc3). Measured with
knfsd and the NFS client in one UML kernel over loopback, NFSv4.2, `sec=sys`,
delegations off, an xfs-backed export
(`scripts/00-run-xfstests-in-uml.sh`, `NFS_DELEGATIONS=0`).

## The symptom

`coverage xfstests (delegations-off)` in CI run 37436016277 and again in
37480407479, each time with every other leg green:

```
QA output created by 095
+fio: io_u error on file /mnt/nfs-test-env/scratch/file2: Device or resource busy: read offset=358400, buflen=1024
 Silence is golden
Failures: generic/095
Failed 1 of 641 tests
```

`buflen=1024` identifies the job. `generic/095` runs nine concurrent fio
jobs over two files, and `job5` is the only 1 KiB reader. Unlike `job1` it
has no `direct=1`, so it is a plain buffered read:

```
[job5]
bs=1k
ioengine=sync
rw=randread
filename=file1:file2
```

The test does not tolerate the error. Its `--ignore_error=,EIO` ignores EIO
on writes only.

## The path

```
nfs_file_read()                     fs/nfs/file.c:162
  nfs_revalidate_mapping()          fs/nfs/file.c:184
    nfs_clear_invalid_mapping()     fs/nfs/inode.c:1489
      nfs_invalidate_mapping()      fs/nfs/inode.c:1460
        nfs_sync_mapping()          fs/nfs/inode.c:126
        invalidate_inode_pages2()   fs/nfs/inode.c:1471
          folio_unmap_invalidate()  mm/truncate.c:622, called at mm/truncate.c:727
            filemap_release_folio() -> nfs_release_folio()   fs/nfs/file.c:512
```

`invalidate_inode_pages2_range()` is documented "Return: -EBUSY if any pages
could not be invalidated". `folio_unmap_invalidate()` has three `-EBUSY`
exits, at `mm/truncate.c:638`, `:640` and `:662`. The one that fires here is
`:640`:

```c
	if (!filemap_release_folio(folio, gfp))
		return -EBUSY;
```

Neither `nfs_invalidate_mapping()` nor `nfs_clear_invalid_mapping()` retries
or translates the error, so it arrives at `nfs_file_read()`, which returns it
in place of reading:

```c
	result = nfs_revalidate_mapping(inode, iocb->ki_filp->f_mapping);
	if (!result) {
		result = generic_file_read_iter(iocb, to);
		...
	}
	nfs_end_io_read(inode);
	return result;
```

## Why nfs_release_folio() fails

`nfs_release_folio()` returns false while the folio still has an `nfs_page`
attached (`fs/nfs/file.c:512`):

```c
	if (folio_test_private(folio)) {
		if ((current_gfp_context(gfp) & GFP_KERNEL) != GFP_KERNEL ||
		    current_is_kswapd() || current_is_kcompactd())
			return false;
		if (nfs_wb_folio_reclaim(folio->mapping->host, folio) < 0 ||
		    folio_test_private(folio))
			return false;
	}
```

`nfs_wb_folio_reclaim()` deliberately does not wait. It returns `-EBUSY`
outright if the folio is under writeback, and otherwise starts a writeback or
an asynchronous `nfs_commit_inode(inode, 0)` and returns. The
`folio_test_private()` recheck that follows then fails whenever the request is
still attached, which under load is the normal case.

That form dates from `cce0be6eb497` ("NFS: Fix a deadlock involving
nfs_release_folio()", Trond Myklebust, 2025-12-31):

```c
-		if (nfs_wb_folio(folio->mapping->host, folio) < 0)
+		if (nfs_wb_folio_reclaim(folio->mapping->host, folio) < 0 ||
+		    folio_test_private(folio))
 			return false;
```

The commit it replaces, `96780ca55e3c` ("NFS: fix up nfs_release_folio() to
try to release the page", 2023-01-19), called `nfs_wb_folio()`, which writes
the folio back and waits, so the private data was gone and the release
succeeded. `cce0be6eb497` carries CVE-2026-23053, fixes a real reclaim
deadlock reported by Wang Zhaolong, and was backported to stable. Nothing
since references it, and no commit in `fs/nfs/` after it revisits the EBUSY
consequence.

## read() propagates it, write() discards it

The same error on the write path is thrown away (`fs/nfs/file.c:775`, inside
`nfs_file_write()`):

```c
	nfs_clear_invalid_mapping(file->f_mapping);
```

No assignment, no check. That call was added unchecked by `28aa2f9e73e7`
("NFS: Always clear an invalid mapping when attempting a buffered write",
2021-02-08), whose stated purpose is that read-modify-write needs a valid
cache. It also runs before `nfs_start_io_write()` takes `inode->i_rwsem`, so
unlike the read path it is not serialised against other writers, which is why
the write path sees the failure far more often.

The asymmetry is the argument that this is a bug rather than a meaningful
result. One caller treats the condition as fatal and reports it to userspace,
the other treats it as ignorable. `read(2)` on a regular file has no
POSIX-defined `EBUSY`, the condition is transient, and a retry succeeds.

Separately, a buffered write proceeding after a failed invalidation does
read-modify-write against a cache it has just been told is stale. That looks
like a second problem, so the write path is not a model for fixing the read
path.

## Upstream has called this a bug before

`874f946376de` ("NFS: Fix a regression in the read() syscall", Trond
Myklebust, 2015-03-02):

> When invalidating the page cache for a regular file, we want to first sync
> all dirty data to disk and then call `invalidate_inode_pages2()`. The latter
> relies on `nfs_launder_page()` and `nfs_release_page()` to deal respectively
> with dirty pages, and unstable written pages.
>
> When commit `9590544694bec` ("NFS: avoid deadlocks with loop-back mounted
> NFS filesystems.") changed the behaviour of `nfs_release_page()`, then it
> made it possible for `invalidate_inode_pages2()` to fail with an EBUSY.
> Unfortunately, that error is then propagated back to read().
>
> Let's therefore work around the problem for now by protecting the call to
> sync the data and `invalidate_inode_pages2()` so that they are atomic
> w.r.t. the addition of new writes. Later on, we can revisit whether or not
> we still need `nfs_launder_page()` and `nfs_release_page()`.

So the symptom has been seen, was classified as a regression, was addressed
with a workaround rather than a fix, and the dependence on
`nfs_release_page()` was left in place. The history of that dependence has
since flip-flopped:

| commit | date | effect on `nfs_release_folio()` |
|---|---|---|
| `9590544694be` | 2014-09-24 | made it able to fail, causing the 2015 regression |
| `874f946376de` | 2015-03-02 | workaround: serialise sync and invalidate against new writes |
| `96780ca55e3c` | 2023-01-19 | on private data, write back synchronously, then release |
| `cce0be6eb497` | 2025-12-31 | back to not waiting, plus an explicit `folio_test_private()` recheck |

The 2015 workaround does not cover the current failure. It serialises against
the *addition* of new writes, whereas `cce0be6eb497` makes the release fail
for writes that are merely not finished yet.

## Evidence

| Claim | How it was checked |
|---|---|
| The tests themselves pass; only the read fails | `Passed all 641 tests` in the v4.0 leg of run 37436016277; in 37480407479 the only failure is the fio read error |
| The failing I/O is the buffered 1 KiB reader | `buflen=1024`, and `job5` is the only 1k job and has no `direct=1` |
| The condition is live and frequent | `nfs_invalidate_mapping` tracepoints: 11,336 calls with 80 `-EBUSY` in one 40-iteration run, 91 in a second |
| The failing branch is the folio release | a `pr_info_once()` on each `-EBUSY` exit of `folio_unmap_invalidate()`; only `release_folio failed` ever printed |
| Writers hit it and discard it | 8 sampled stacks, all `nfs_file_write()` -> `nfs_clear_invalid_mapping()` |
| The read path can hit it | stack dump at the `-EBUSY` return showing `nfs_file_read()` -> `nfs_revalidate_mapping()`, in the iteration that failed |
| CI and the local kernel are the same code here | `a90ee4305c4a` is 1,708 commits ahead of `4982d3552` with no changes under `fs/nfs/`, `mm/truncate.c` or `mm/filemap.c` |

## Reproducing

Unmodified, the read-side failure is rare: 80 iterations of `generic/095`
under UML passed while logging 171 discarded `-EBUSY` invalidations, all on
the write path. CI hit the read path roughly once in 24 leg runs.

Two `msleep()` calls widen windows that already exist and make it reproduce in
about a dozen iterations. Neither changes locking or error handling.

In `nfs_vm_page_mkwrite()` (`fs/nfs/file.c:681`), after the existing wait that
is a check with no lock held:

```c
 	wait_on_bit_action(&NFS_I(inode)->flags, NFS_INO_INVALIDATING,
 			   nfs_wait_bit_killable,
 			   TASK_KILLABLE|TASK_FREEZABLE_UNSAFE);
+	msleep(1);
 	folio_lock(folio);
```

In `nfs_invalidate_mapping()` (`fs/nfs/inode.c:1460`), between the sync and
the invalidate:

```c
 			ret = nfs_sync_mapping(mapping);
 			if (ret < 0)
 				return ret;
 		}
+		msleep(2);
 		ret = invalidate_inode_pages2(mapping);
```

Then:

```sh
NFS_DELEGATIONS=0 bash scripts/00-run-xfstests-in-uml.sh -I 40 generic/095
```

It failed on iteration 12 with the CI symptom. Adding a `dump_stack()` where
`nfs_invalidate_mapping()` returns `-EBUSY` gives the chain:

```
nfs_clear_invalid_mapping+0x1f3/0x21d
nfs_revalidate_mapping+0x38/0x5e
nfs_file_read+0xbe/0x128
vfs_read+0x10b/0x1ae
ksys_read+0x7b/0xaf
sys_read+0x14/0x16
```

Timestamps line up: iteration 12 started at `261.15`, the read-path `-EBUSY`
fired at `268.72`, and iteration 12 is the one that failed. UML stack traces
need `DEBUG_KERNEL` and `FRAME_POINTER`, or `get_frame_pointer()` returns 0
(`arch/um/include/asm/stacktrace.h`) and every frame prints unreliable.

A purpose-built reproducer, an mmap writer plus an O_DIRECT writer plus a
buffered reader, did **not** reproduce it: 5,934 invalidations, zero failures,
even with both windows widened. The O_DIRECT writer is needed for any
invalidation at all, because a direct write's completion zaps the mapping
(`nfs_zap_mapping()`, `fs/nfs/direct.c:733`); without it the client attributes
the change attribute to its own buffered writes and never invalidates. Even
so, the nine-job mix in `generic/095` is what produces the failure, and which
of its writers leaves the attached request behind is not established.

## What a fix would have to do

Making `nfs_release_folio()` wait again would reintroduce the deadlock
`cce0be6eb497` fixed, so that is not available. Ignoring the error on the read
path would serve stale data, which is the opposite of what the invalidation is
for. That leaves retrying the invalidation, or completing the writeback and
commit of the offending folios synchronously in a context where that is safe,
which reclaim is not and a `read(2)` is. This is an assessment, not something
upstream has proposed.

## Status

Not reported upstream. The CI legs that hit it are the NFSv4.2 ones;
`generic/095` is in `-g quick` and in no exclude file, so every variant runs
it and any of them can flake.
