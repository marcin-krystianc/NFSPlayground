# VAST NFS 4.5.8 compared to Linux v6.12.57

How the VAST NFS source relates to the mainline kernel code it replaces.
All numbers below were measured, not estimated. See [Method](#method) to reproduce.

## Baseline

VAST NFS is a maintained fork of five kernel subtrees, built out of tree.

The upstream baseline is Linux **v6.12.57**. This is not an inference:
`vastnfs-4.5.8/scripts/BASE` contains `v6.12.57`, and `scripts/sync-from-linux.sh`
aborts unless the Linux checkout is exactly that commit:

    Checked out version in ${linux_source} is not ${base}

`scripts/sync-from-linux.sh <linux-source> <base>` copies upstream files into
`bundle/`. `scripts/OLDER` is a hand-annotated ledger of VAST commits marked
`already applied`, `not relevant`, or `irrelevant` during forward rebases.

## Replacement, not coexistence

`bundle/` mirrors the upstream layout exactly, with the same filenames and the
same module names: `fs/nfs` (+ `filelayout`, `blocklayout`, `flexfilelayout`),
`fs/nfsd`, `fs/lockd`, `fs/nfs_common`, `net/sunrpc` (+ `auth_gss`, `xprtrdma`),
`include/linux/{nfs*,sunrpc}`.

The built modules are installed under `extra/vastnfs/bundle/` (RPM) or
`updates/bundle/` (DEB) and a depmod rule makes them win over the inbox
modules of the same name. `debian/depmod.conf` is a single line:

    search updates/bundle

`vastnfs.spec` `%files` ships only the modules, `/usr/bin/vastnfs-ctl`, and
`/usr/share/vastnfs/build-info.txt`. No userspace NFS component is replaced, so
stock `nfs-utils` drives a VAST client unchanged.

## Size of the delta

Changed lines counted as `diff -u | grep -c '^[+-]'`. The identical column
counts top level files in each directory that are byte for byte equal to
upstream.

| subtree           | changed lines | top level files | byte identical |
| ----------------- | ------------: | --------------: | -------------: |
| `fs/nfs`          |          5470 |              76 |             30 |
| `net/sunrpc`      |          4059 |              35 |             12 |
| `fs/nfsd`         |           700 |              55 |             36 |
| `fs/lockd`        |           395 |              21 |              6 |
| `fs/nfs_common`   |            19 |               6 |              3 |

Largest single files:

| file                          | changed lines |
| ----------------------------- | ------------: |
| `fs/nfs/nfstrace.h`           |          1055 |
| `net/sunrpc/clnt.c`           |           890 |
| `fs/nfs/fs_context.c`         |           666 |
| `fs/nfs/nfs4proc.c`           |           571 |
| `net/sunrpc/rpc_pipe.c`       |           549 |
| `net/sunrpc/xprt.c`           |           373 |
| `net/sunrpc/xprtmultipath.c`  |           326 |
| `fs/nfs/inode.c`              |           304 |
| `net/sunrpc/debugfs.c`        |           297 |
| `fs/nfs/super.c`              |           271 |
| `fs/nfs/delegation.c`         |           260 |
| `net/sunrpc/xprtsock.c`       |           234 |

## Files only in the bundle

Not present in upstream v6.12.57:

| file                                       | purpose                                  |
| ------------------------------------------ | ---------------------------------------- |
| `net/sunrpc/jobid.c`, `jobid.h`            | job ID tagging (`#VastMD=` machine name) |
| `net/sunrpc/addr_external.c`               | address helpers                          |
| `net/sunrpc/rpcrdma_dummy.c`               | RDMA build without RDMA support          |
| `net/sunrpc/xprtrdma/nvfs*.{c,h}`          | Nvidia GDS integration                   |
| `net/sunrpc/xprtrdma/debugfs.c`            | transport introspection                  |
| `fs/nfs/debugfs.c`                         | client introspection                     |
| `fs/nfs/nfs4xdrtrace.h`, `nfsxdrtrace.h`, `nfsmount_xdrtrace.h` | XDR tracepoints      |
| `fs/lockd/nlm4xdrtrace.h`, `nlmxdrtrace.h`, `nsmxdrtrace.h`     | XDR tracepoints      |
| `net/sunrpc/rpcb_xdrtrace.h`               | XDR tracepoints                          |
| `fs/nfsd/fault_inject.c`, `fault_inject.h` | present in bundle, absent upstream       |
| `net/sunrpc/Makefile.lib`, `bundle/bin-wrap` | out of tree build plumbing             |

Files only upstream, within the five subtrees:

- `fs/nfs_common/grace.c`
- `net/sunrpc/.kunitconfig`

## Where multipath lives

Upstream is not without multipath infrastructure. At v6.12.57 mainline already
has `net/sunrpc/xprtmultipath.c` (17 KB, the `rpc_xprt_switch` machinery that
lets one `rpc_clnt` own several transports) and the `nconnect=` / `max_connect=`
mount options (`fs/nfs/fs_context.c:171-172`). What upstream lacks is multiple
destination IPs and selectable source IPs per mount: upstream `nconnect` opens N
connections to the same server address.

Core structure, `bundle/include/linux/sunrpc/clnt.h:141`:

```c
#define RPC_MAX_PORTS 2048

struct rpc_portgroup {
	int nr;
	bool dns;
	struct sockaddr_storage addrs[RPC_MAX_PORTS];
	size_t lens[RPC_MAX_PORTS];
};
```

`localports` and `remoteports` fields are added to `struct rpc_create_args`.
`struct xprt_portusage` is defined at `bundle/include/linux/sunrpc/xprt.h:394`
and has zero occurrences anywhere in upstream.

Distribution of multipath symbols (`rpc_portgroup`, `localports`, `remoteports`,
`local_ports`, `remote_ports`) across the bundle:

| file                                | hits | role                                                    |
| ----------------------------------- | ---: | ------------------------------------------------------- |
| `net/sunrpc/clnt.c`                 |   61 | transport creation, remote port pick, local port bind    |
| `fs/nfs/fs_context.c`               |   37 | parses `remoteports=`, `localports=`, `pconnect=`, `dns` |
| `fs/nfs/client.c`                   |   35 | carries port groups into `nfs_client`, client matching   |
| `fs/nfs/nfs4client.c`               |   31 | NFSv4.1 fan out into multiple clients                    |
| `include/linux/sunrpc/clnt.h`       |   16 | `rpc_portgroup` and API                                  |
| `fs/nfs/internal.h`                 |   11 | context plumbing                                         |
| `fs/nfs/super.c`                    |    8 | mount option display                                     |
| `include/linux/nfs_fs_sb.h`         |    6 | `nfs_client` fields                                      |
| `net/sunrpc/stats.c`                |    4 | extended `mountstats` output                             |
| `net/sunrpc/debugfs.c`              |    4 | `rpc_clnt/<id>/stats`                                    |
| `net/sunrpc/xprtmultipath.c`        |    2 | upstream machinery, lightly touched                      |
| `include/trace/events/sunrpc.h`     |    2 | tracepoints                                              |

Four functional hot spots:

1. **Option parsing.** `nfs_parse_port_group()`, `bundle/fs/nfs/fs_context.c:785`.
   Implements the `-` range and `~` union grammar and `remoteports=dns`.
   Pure logic with no I/O, so the best KUnit target in the tree.
2. **Remote port selection.** `rpc_calc_portgroup_offset()`,
   `bundle/net/sunrpc/clnt.c:651`, called from `clnt.c:907`. Hashes the local
   source address into the port group unless `remoteports_offset=` is given.
   This is what spreads a 32 address range across `nconnect=8` transports.
3. **Local port binding.** `bundle/net/sunrpc/clnt.c:885-940`. Iterates
   `args->localports`, binds each transport to a different source address round
   robin, and sets `diversion_enabled` for `localports_failover`.
4. **NFSv4.1 client fan out.** `bundle/fs/nfs/nfs4client.c:1274-1311`. NFSv4.1
   cannot spread one client's session across addresses, so VAST creates
   `ceil(nconnect / pconnect)` separate `nfs_client` contexts, one per target IP.
   Worked examples are in the source comment, for example
   `nconnect=16,pconnect=4,remoteports=4` gives 4 ports with 4 connections each.

VAST reuses upstream's transport switch rather than replacing it, and adds the
address selection layer above it in `clnt.c` and in NFS client setup.

## Three kinds of change

The delta is not uniform, and each kind needs a different test strategy.

1. **VAST features.** Multipath, job ID tagging, GDS, extra debugfs and
   tracepoints. No upstream equivalent, therefore no upstream test covers them.
2. **Backport.** Linux 6.12 NFS code made to compile on kernels from 4.15 to
   7.0. Behaviour is meant to equal upstream, so the risk is version conditional
   breakage rather than new semantics. This is what xfstests `-nfs` and pynfs
   are for.
3. **Compat plumbing.** `compat/` holds 124 feature probes (`compat/checks/*.c`,
   for example `d_hash_and_lookup_6_16.c`, `folio_end_read_6_7.c`) plus shim
   headers. This is already a compile time test suite, and a probe misfiring on
   some distro kernel is a common failure mode. Cheapest suite to run.

## Existing test surface in the bundle

`bundle/net/sunrpc/auth_gss/gss_krb5_test.c` is an upstream KUnit module, 1867
lines, wired up as `obj-$(CONFIG_RPCSEC_GSS_KRB5_KUNIT_TEST)` in
`bundle/net/sunrpc/auth_gss/Makefile:17` and declared tristate in
`bundle/net/sunrpc/Kconfig:75`. Upstream's `net/sunrpc/.kunitconfig` was not
carried over.

VAST's own CI (`scripts/ci-test.sh`) is not reusable: it clones a private GitLab
repo `driver-ops` and execs `ops ci-entry`. It pins nfs-utils to
`nfs-utils-2-6-1-6.GR-8pre2`.

## Method

Ran on 2026-08-19 against `vastnfs-4.5.8.tar.xz`
(sha256 `a4abaf2d6034d2b9d8d42086c30c355b2baf680ee3fb8e53a63af969f32d3b52`,
see `vastnfs/fetch.sh`).

```sh
tar xf vastnfs/vastnfs-4.5.8.tar.xz

git clone --filter=blob:none --sparse --depth 1 --branch v6.12.57 \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux
cd linux
git sparse-checkout set fs/nfs fs/nfsd fs/lockd fs/nfs_common \
    net/sunrpc include/linux/nfs include/linux/sunrpc
cd ..

L=linux V=vastnfs-4.5.8/bundle
for d in fs/nfs fs/nfsd fs/lockd fs/nfs_common net/sunrpc; do
    echo -n "$d: "
    diff -ru --exclude='.*' $L/$d $V/$d | grep -c '^[+-]'
done

diff -rq $L $V | grep "^Only in $V"
```

The sparse clone cost 270 MB in `.git` and a 24 MB working tree, about one
minute. Extracting the VAST tree requires a case sensitive filesystem: it ships
both `Makefile` and `makefile` in the root and in each `src/v*` directory.
