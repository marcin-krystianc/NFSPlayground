# VAST NFS multipath: what happens when a node goes offline

Measured on the test VM (Ubuntu 24.04, kernel 6.8.0-138-generic, VAST NFS
4.5.8) against the three-node docker cluster from
[`scripts/nfs-test-env/`](../scripts/nfs-test-env), on 2026-08-26. Every
number below was observed, not estimated; the reproduction steps are at the
end.

## Summary

`remoteports=` spreads RPC transports across multiple servers, and that part
works. It is **not** a high-availability mechanism. Under both NFSv3 and
NFSv4.1, requests assigned to a transport whose destination node stops
responding **hang until that node returns** — the client does not move them
to a surviving transport. Traffic on the remaining transports is unaffected,
so the observable effect is a *partial* outage roughly proportional to the
share of transports pointing at the dead node, sustained indefinitely.

Recovery is automatic and complete once the node comes back.

## What was measured

Setup: 4 transports (`nconnect=4`) spread over three nodes, distributed
2/1/1. Node `172.28.0.11` (2 of the 4 transports) was blackholed with
`iptables -j DROP` on both directions — simulating a node that vanishes
without closing its TCP connections, which is the hard case. A writer
issued one 4 KiB `O_DIRECT` write to a fresh file every 200 ms, each with a
5 s timeout, so a stalled write reports `FAIL` instead of hanging forever.

| | NFSv3 | NFSv4.1 |
|---|---|---|
| Baseline write latency | ~13 ms, 0 failures | ~14 ms, 0 failures |
| First failure after blackhole | +6 s | +6 s |
| Sustained failure rate | 19/60 (~32%) at 90 s | 11/50 (~22%) at 2 min |
| Did it route around the dead node? | **No** | **No** |
| Transport state while node dead | still `CONNECTED BOUND` | `LOCKED CONNECTING BOUND` |
| Recovery when node returned | full, 40/40 ok | full |

Every failing write consumed exactly the 5 s timeout (`5.0018…`), meaning
it would have blocked indefinitely on a `hard` mount without it.

The one behavioural difference between versions: NFSv4.1 *notices* the dead
peer and moves those transports to `CONNECTING`, whereas NFSv3 leaves them
marked `CONNECTED` for the entire outage. Neither, however, stops assigning
new requests to them.

## Why — traced in the source

Moving an in-flight RPC to another transport happens in exactly one place,
`bundle/net/sunrpc/clnt.c:2789-2790`, and it is gated on **two** conditions:

```c
if (!(task->tk_flags & RPC_TASK_NO_ROUND_ROBIN) &&
    (task->tk_flags & RPC_TASK_MOVEABLE) &&
    test_bit(XPRT_REMOVE, &xprt->state)) {
```

- `RPC_TASK_MOVEABLE` comes from `NFS_CAP_MOVEABLE`, which appears in only
  two capability sets — `nfs_v4_1_minor_ops` and `nfs_v4_2_minor_ops`
  (`bundle/fs/nfs/nfs4proc.c:10962` and `:10998`). So on NFSv3 and NFSv4.0
  the flag is never set and migration is impossible by construction.
- `XPRT_REMOVE` is set when a transport is being *removed* — a trunking or
  administrative change. A network blackhole never sets it. This was
  confirmed empirically: the transports to the dead node kept reporting
  `CONNECTED`/`CONNECTING`, never a removed state.

That second condition is why NFSv4.1 behaved no better than NFSv3 here
despite being moveable: the flag is necessary but not sufficient, and the
state bit that would unlock migration is never reached by node loss alone.

The same two-condition gate appears in `rpc_task_set_transport()`
(`clnt.c:1702-1706`) for re-picking a transport on an already-bound task.

`RPC_XPRT_FLAGS_SKIP_UNCONNECTED` (`clnt.c:1699`) looks like it should make
*new* requests avoid dead transports, but reading `xprt_is_active()`
(`bundle/net/sunrpc/xprtmultipath.c:271-275`) it only skips transports with
a nonzero `xprt_n_diversion()` or `xprt_n_recovery()` counter — VAST's
diversion machinery, which is driven by `localports` (client-side address
failover, documented RDMA-only) and not by a remote node dying. A merely
unresponsive transport still looks selectable, which matches the sustained,
non-decaying failure rate observed.

## What this means in practice

- `remoteports=` buys **bandwidth aggregation across servers, not
  availability**. Do not treat it as failover.
- The blast radius of one dead node is its share of `nconnect` transports.
  With 4 transports over 3 nodes, losing the node holding 2 of them cost
  roughly a third of writes, indefinitely.
- A node that reboots and returns on the same address is fully survivable —
  the mount heals with no intervention. A node that is permanently gone
  strands its share of traffic until the mount is torn down.
- `soft` mounts would convert these hangs into `EIO` after the retransmit
  budget rather than blocking, at the usual cost of surfacing errors to
  applications.

## Limits of this measurement

- Three single-node `nfsd` containers over a shared backing store are not a
  real VAST cluster — see [What the test cluster actually
  is](#what-the-test-cluster-actually-is) for exactly which cluster
  properties it does and does not have. A real cluster has server-side
  machinery this lacks, which may change the picture.
- The NFSv4.1 numbers carry an extra caveat: `nfsd` state (client IDs,
  sessions, stateids) lives per network namespace, so each container has
  its own. How a single v4.1 mount spreading transports over three such
  servers works at all is **not understood** — it was observed to work, not
  explained. Until it is, treat the v4.1 row as less trustworthy than the
  NFSv3 one, whose statelessness makes it unambiguous.
- Only the blackhole failure mode was tested. A node that closes its
  connections cleanly (`docker stop`, or a graceful shutdown sending RST)
  may behave differently, since TCP would signal the failure immediately —
  worth testing separately.
- `localports_failover` was not exercised: it is documented as RDMA-only
  and this environment is TCP.
- One anomaly seen once and **not reproduced**: a `vers=4.1` mount with
  `pconnect=2` had all writes hang at baseline with no node failure
  injected. A later rigorous retest of the same option combination passed.
  Not claimed as a bug; noted in case it recurs.

## Making the test cluster coherent (prerequisite)

None of the above was measurable until the docker servers were changed from
independent exports to a shared backing store. Previously each container
exported its own directory, so a filehandle issued by one node was
meaningless on another and any spread I/O failed with `ESTALE` — even `df`
failed once traffic crossed servers.

Linux `nfsd` filehandles encode fsid + inode + generation, so exporting the
**same** host directory from every node with an **identical** `fsid=` makes
handles portable. Verified with xfstests' own `src/open_by_handle`:

```sh
# handles saved from node A open successfully against node B
open_by_handle -c -o /tmp/handles.a /mnt/fh-a 3
open_by_handle -i /tmp/handles.a /mnt/fh-b 3     # SUCCESS

# same handles against a node with independent backing storage
open_by_handle -i /tmp/handles.a /mnt/fh-c 3     # errno 116 (ESTALE)
```

NFSv4 additionally needs the exported *root* to be shared, not just its
children, or the pseudo-root filehandle differs per node. `01-setup-servers.sh`
therefore bind-mounts one host directory as `/export` (`fsid=0`) with
`test` (`fsid=1`) and `scratch` (`fsid=2`) beneath it.

Note the path asymmetry this creates: NFSv3 mounts `server:/export/scratch`,
NFSv4 mounts `server:/scratch` (relative to the pseudo-root).

## What the test cluster actually is

Calling these three containers a "cluster" overstates it. They are three
NFS **front-ends onto one filesystem**: separate `nfsd` instances, each in
its own network namespace, all bind-mounting the same host directory and
therefore all operating on the same inodes in the same host kernel.

That shared-kernel, shared-inode arrangement gives more than expected. The
following were measured, not assumed:

| Property | Holds? | How it was checked |
|---|---|---|
| Filehandles portable across nodes | yes | `open_by_handle -o` on A, `-i` on B succeeds; ESTALE (116) against independent backing |
| POSIX locks conflict across nodes | yes | exclusive `fcntl` lock held via node A; same lock via node B returns `EAGAIN` (11) |
| Writes via one node visible via another | yes | write via A, immediate read via B returns the new content |
| New files visible across nodes | yes | `touch` via A, appears in `ls` via B |

The reason is mundane: locks and page cache are enforced by the one host
kernel on the one shared inode, not by any cluster protocol. It is an
accident of the setup, not an emulation of clustering — but it is enough to
make NFSv3 data-path multipath behave sensibly.

What it still is **not**:

- **No availability.** A dead node's share of traffic hangs; that is the
  whole subject of this document. Nothing reroutes or reclaims.
- **No shared NFSv4 state.** `nfsd` client IDs, sessions and stateids are
  per network namespace, so each container has its own. See the caveat
  above about the v4.1 measurements.
- **No shared duplicate reply cache.** A retried non-idempotent operation
  landing on a different node would not be recognised as a replay. Not
  measured; flagged as a known gap.
- **No lock-recovery coordination.** Grace periods and `sm-notify` state
  are per-server, so a node's locks are not reclaimable elsewhere when it
  dies. Not measured.

An earlier version of this document asserted that lock state and cache
coherence were *not* shared here. Both claims were wrong and were corrected
after being tested.

## Reproducing

```sh
# three-node cluster + VAST driver loaded (see xfstests-against-vastnfs.md)
bash scripts/nfs-test-env/01-setup-servers.sh
sudo mount -t nfs -o vers=3,nconnect=4,remoteports=172.28.0.11-172.28.0.13 \
    172.28.0.12:/export/scratch /mnt/nfs-test-env/scratch

# confirm the spread, then start a write probe against the mount
sudo vastnfs-ctl rpc-transports /mnt/nfs-test-env/scratch | grep dstaddr

# blackhole whichever node holds transports, and watch writes stall
sudo iptables -I OUTPUT -d 172.28.0.11 -j DROP
sudo iptables -I INPUT  -s 172.28.0.11 -j DROP
# ... observe ...
sudo iptables -D OUTPUT -d 172.28.0.11 -j DROP
sudo iptables -D INPUT  -s 172.28.0.11 -j DROP
```

Each write in the probe used `dd ... bs=4k count=1 oflag=direct` under
`timeout 5`; `O_DIRECT` matters, because buffered writes land in the page
cache and hide the stall until writeback.
