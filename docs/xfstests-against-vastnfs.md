# Running xfstests against VAST NFS kernel modules

Run xfstests against vanilla NFS servers in Docker with the inbox kernel
client, then swap in the VAST NFS kernel modules and run the same tests
again.

This is a Docker-for-servers, host-kernel-for-client setup: mounts happen on
the VM host, not inside a container, because the kernel module doing the
mounting (inbox or VAST) is a host-wide thing — containers share the host
kernel, so there is no per-container isolation of "which NFS client is
active."

See also:

- [vastnfs-multipath-failover.md](vastnfs-multipath-failover.md) — measured
  behaviour when a node goes offline under `remoteports=` (short version:
  it does not fail over), and why the servers are set up as a shared-backing
  "cluster" rather than independent exports.
- [xfstests-vs-pynfs.md](xfstests-vs-pynfs.md) — what xfstests actually
  covers vs. protocol-level testing.
- [testing-vast-features-without-hardware.md](testing-vast-features-without-hardware.md)
  — which VAST features need real hardware (only GDS does) and which don't.
- [vastnfs-vs-linux.md](vastnfs-vs-linux.md) — what's actually different in
  the VAST bundle vs. upstream.

## Prerequisites

- A Linux VM with a kernel in the range listed in
  `vastnfs-4.5.8/docs/src/build/kernels.md` (e.g. Ubuntu 24.04 LTS, GA
  kernel 6.8, already in range with no HWE install needed).
- Docker installed on that VM, and your user in the `docker` group
  (`sudo usermod -aG docker $USER`, then re-login — group changes don't
  apply to an already-open session).
- Passwordless sudo for the account running this. The run script calls
  `sudo ./check`, and installing the VAST package calls `sudo dpkg -i`.
  A drop-in is enough: `echo "$USER ALL=(ALL) NOPASSWD:ALL" | sudo tee
  /etc/sudoers.d/$USER-nopasswd && sudo visudo -c`. Without it, `sudo`
  blocks on a TTY password prompt the scripts don't provide.
- Build tooling for the VAST package: `build-essential dkms autoconf
  automake libtool pkg-config debhelper dh-dkms` at minimum — `dh-dkms`
  and `debhelper` aren't pulled in by `build-essential`, and `./build.sh
  bin` fails on `Unmet build dependencies: debhelper` without them.
- Dependencies for building xfstests itself: `libaio-dev libattr1-dev
  libacl1-dev uuid-dev xfslibs-dev libgdbm-dev libtool-bin e2fsprogs
  libblkid-dev libssl-dev libdevmapper-dev git bc fio dbench attr xfsprogs
  nfs-common quota nfs4-acl-tools rpm`.
- `vastnfs-4.5.8/` and `linux/` fetched — `scripts/fetch-sources.sh`.
- The host kernel's `nfs`/`nfsd` modules loaded before the first run
  (`sudo modprobe nfs && sudo modprobe nfsd`) — the `erichough/nfs-server`
  image's entrypoint checks the *host's* loaded modules (containers share
  the host kernel) and refuses to start otherwise.

## Step 1 — baseline against the inbox client

```sh
bash scripts/00-run-xfstests-on-vm-and-docker.sh generic/001 generic/002
# or: bash scripts/00-run-xfstests-on-vm-and-docker.sh -g quick
```

One self-contained script: it brings up the docker NFS servers, builds
xfstests, writes `xfstests/local.config`, runs the tests, and tears the
servers down again from an `EXIT` trap — so the containers go away whether
the run passes, fails or is interrupted. `xfstests/results/` survives, which
is what matters for diagnosing a failure. It exits with xfstests' own
status.

Whatever kernel is loaded at this point (inbox, initially) is what gets
exercised. Record the results — this is what you diff against once the VAST
modules are in.

Every value is overridable from the environment, e.g.
`NFS_SERVER_COUNT=4 bash scripts/00-run-xfstests-on-vm-and-docker.sh`.

### What it stands up

A docker bridge network (`172.28.0.0/24` by default) and one privileged
`erichough/nfs-server` container per server, each with a static IP.

All servers export the **same** host directory
(`$HOME/nfs-test-env-exports/root` by default, no sudo needed — move it with
`NFS_EXPORT_BASE`) with identical `fsid=` values, which makes them behave as
one crude cluster: a filehandle issued by any node is valid on every other
node, so `remoteports=` can actually spread one mount's traffic across all
of them. With independent per-server directories, spread I/O fails with
`ESTALE`. The evidence for that, and what this still doesn't simulate
(shared lock state, cache coherence), is in
[vastnfs-multipath-failover.md](vastnfs-multipath-failover.md).

The export root is bind-mounted, not just its children, so the NFSv4
pseudo-root filehandle is identical across nodes too — a container-local
root would have a different inode per container and break v4 multipath at
the root.

Layout: `/export` (`fsid=0`, the NFSv4 pseudo-root), `/export/test`
(`fsid=1`), `/export/scratch` (`fsid=2`). Mind the path asymmetry — NFSv3
mounts `server:/export/scratch`, NFSv4 mounts `server:/scratch`.

## Step 2 — build and install the VAST kernel modules

```sh
cd vastnfs-4.5.8
./build.sh bin
sudo dpkg -i dist/vastnfs-modules_4.5.8-vastdata.kver.$(uname -r)_amd64.deb
```

Per `vastnfs-4.5.8/docs/src/build/package.md` and `docs/src/INSTALL.md`.
This is a host-wide kernel module replacement — see
[vastnfs-vs-linux.md](vastnfs-vs-linux.md)'s "Replacement, not
coexistence". `build.sh` refuses outright if the running kernel is outside
the supported range in `docs/src/build/kernels.md`; check that first rather
than discovering it after a long build.

### Loading them: reboot, or reload live

Installing does not load the new modules — the old ones stay resident until
something reloads them.

**Reboot** is what `vastnfs-4.5.8/docs/src/INSTALL.md` documents.

**Or reload live**, using `vastnfs-ctl reload`
(`vastnfs-4.5.8/docs/src/usage/vastnfs-ctl.md`), verified working on the
test VM:

```sh
# Stop anything holding nfsd open first (e.g. the docker NFS servers, which
# use the host's shared nfsd module) -- otherwise reload fails with
# "rmmod: ERROR: Module nfsd is in use".
docker stop nfs-test-env-1 nfs-test-env-2 nfs-test-env-3

sudo vastnfs-ctl reload

# reload only reloads modules that were in use; if nothing had `nfs` (the
# client module) mounted, it stays unloaded -- pull it back in so the next
# mount, and the containers' healthcheck, pick up the VAST build.
sudo modprobe nfs

docker start nfs-test-env-1 nfs-test-env-2 nfs-test-env-3
```

If `nfsd` gets unloaded while a container still holds it open, `docker stop`
the containers first, reload, then `sudo modprobe nfs` before `docker
start`-ing them — otherwise the container's healthcheck fails with "kernel
module nfs is missing", because it is checking the *host's* loaded modules.

Confirm which build is loaded with `modinfo nfs | grep filename`: VAST
modules live under `.../updates/bundle/...`, inbox ones under the normal
kernel module tree.

### Verify the swap took

```sh
sudo vastnfs-ctl status
```

Cross-check the loaded `sunrpc` module's `srcversion` against the freshly
built one, per `vastnfs-4.5.8/docs/src/INSTALL.md`'s own verification
method. If they don't match, the new modules are not the ones running yet.

## Step 3 — re-run the same tests

```sh
bash scripts/00-run-xfstests-on-vm-and-docker.sh generic/001 generic/002
```

Same command, same servers, same config — the only thing that changed is
which kernel modules handle the client side. Diff against step 1.

## Step 4 — exercise VAST-specific multipath

`xfstests/tests/nfs/002` is the automated case. It mounts scratch with
`remoteports=<first-server-ip>-<last-server-ip>` across all the docker
servers, then counts the distinct `dstaddr=` values reported by
`vastnfs-ctl rpc-transports`. More than one means traffic really is spread
across multiple destination IPs (the VAST addition) rather than opening N
connections to a single address (upstream `nconnect` behaviour).

It `_notrun`s automatically on a non-VAST client (via
`/sys/module/sunrpc/parameters/nfs_bundle_version`) or without a
`NFS_MULTIPATH_REMOTEPORTS` range in `local.config` — which the run script
writes for you.

```sh
bash scripts/00-run-xfstests-on-vm-and-docker.sh nfs/002
# or the whole VAST group: bash scripts/00-run-xfstests-on-vm-and-docker.sh -g vastnfs
```

It proves spread only. It does **not** prove the mount survives losing a
node — measurements showing it does not are in
[vastnfs-multipath-failover.md](vastnfs-multipath-failover.md).

## Teardown

The run script removes its containers and network on exit. It does **not**
uninstall the VAST kernel modules — that is a separate, host-wide step, see
`vastnfs-4.5.8/docs/src/UNINSTALL.md`.

## Known gaps

- `nfs/002` only proves `remoteports=` spreads across destination
  addresses. It does not check `localports=` or `pconnect=`, and does not
  contrast against plain `nconnect=` staying on one address, which would
  strengthen the signal.
- `nfs/001` needs `TEST_DEV` mounted as NFSv4 and is skipped by default,
  since the generated `local.config` does not pin a version. Add `vers=4` to
  a manual mount, or extend the config, to include it.
- Measured on the test VM with the inbox client: 644/644 `-g quick` tests
  passed, 0 failures, the remainder correctly `[not run]` for NFS
  limitations (reflink/dedupe/quotas/dax/encryption/block-device-only
  checks). The same comparison against the VAST modules has not been
  recorded here.
