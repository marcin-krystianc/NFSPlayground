# Testing VAST-specific features without VAST hardware

Which of the VAST-only features listed in
[vastnfs-vs-linux.md](vastnfs-vs-linux.md#three-kinds-of-change) ("VAST
features: multipath, job ID tagging, GDS, extra debugfs and tracepoints")
need a real VAST storage cluster to test, and which don't.

## Multipath (`nconnect`/`pconnect`/`remoteports`/`localports`)

No VAST hardware needed.

The logic lives entirely on the client side: `bundle/net/sunrpc/clnt.c`,
`bundle/fs/nfs/fs_context.c`, `bundle/fs/nfs/nfs4client.c`. It parses mount
options and spreads RPC connections across addresses and ports. It works
against whatever NFS server is on the other end; the server just needs to be
reachable on multiple IPs/ports, which any Linux `nfsd` behind multiple
loopback/veth addresses or network namespaces provides. No VAST cluster
required.

`nfs_parse_port_group()` (`fs_context.c:785`), the option-parsing piece, is
pure logic with no I/O at all — testable with zero server, already flagged
in vastnfs-vs-linux.md as the best KUnit candidate in the tree.

## Job ID tagging (`#VastMD=`)

No VAST hardware needed.

Checked `bundle/net/sunrpc/jobid.c`: it reads a set of env vars from the
calling process (`job_id_vars`, `env_cache`) and prefixes outgoing RPCs with
that string. There is no server-side VAST logic involved on the client side.
Testable against any NFS server, or even without a mount if only the tagging
logic itself is being tested.

## Extra debugfs / tracepoints

No VAST hardware needed.

`fs/nfs/debugfs.c`, `net/sunrpc/xprtrdma/debugfs.c`, and the XDR tracepoint
headers are pure introspection. Observable as soon as the module loads,
under any workload.

## GDS (Nvidia GPUDirect Storage), `net/sunrpc/xprtrdma/nvfs*.c`

Needs real hardware to exercise the actual data path, but not VAST hardware.

GDS does DMA between GPU memory and the network stack, so exercising it for
real needs an Nvidia GPU with GDS support and an RDMA-capable NIC. That
hardware requirement is Nvidia/RDMA-specific, not VAST-specific — any
GDS/RDMA-capable setup works, not a VAST storage cluster.

Checked `bundle/net/sunrpc/rpcrdma_dummy.c`: it's a no-op stub module
(`printk` plus empty init/exit) used so the build succeeds when RDMA isn't
configured. Confirms the tree is meant to *build* without RDMA present —
only the GDS data path itself needs the real hardware, not compilation.

## Conclusion

Everything except GDS is testable against a plain Linux NFS server (loopback,
a VM, containers with multiple IPs). Only the GPUDirect Storage path needs
GPU+RDMA hardware, and even that doesn't have to be VAST's. No part of
testing VAST-specific features requires paying VAST or owning a VAST
cluster.
