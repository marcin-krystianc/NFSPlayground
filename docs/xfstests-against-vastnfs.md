# Running xfstests against VAST NFS kernel modules

Practical walkthrough for `scripts/nfs-test-env/`: run xfstests against
vanilla NFS servers in Docker with the inbox kernel client, then swap in the
VAST NFS kernel modules and run the same tests again.

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
- Passwordless sudo for the account running these scripts (`03-run-xfstests.sh`
  calls `sudo ./check`, and `04-build-install-vastnfs.sh` calls `sudo dpkg -i`).
  A drop-in is enough: `echo "$USER ALL=(ALL) NOPASSWD:ALL" | sudo tee
  /etc/sudoers.d/$USER-nopasswd && sudo visudo -c`. Without it, `sudo` blocks
  on a TTY password prompt these scripts don't provide.
- Build tooling for step 4: `build-essential dkms autoconf automake libtool
  pkg-config debhelper dh-dkms` at minimum — `dh-dkms`/`debhelper` aren't
  pulled in by `build-essential` and `./build.sh bin` fails on
  `Unmet build dependencies: debhelper` without them.
- Dependencies for building xfstests itself: `libaio-dev libattr1-dev
  libacl1-dev uuid-dev xfslibs-dev libgdbm-dev libtool-bin e2fsprogs
  libblkid-dev libssl-dev libdevmapper-dev git bc fio dbench attr xfsprogs
  nfs-common quota nfs4-acl-tools rpm`.
- `xfstests` built (`cd xfstests && make`) — see `xfstests/README`.
- `vastnfs-4.5.8/` and `linux/` checked out —
  `scripts/fetch-sources.sh` if not already done.
- The host kernel's `nfs`/`nfsd` modules loaded before step 1
  (`sudo modprobe nfs && sudo modprobe nfsd`) — the `erichough/nfs-server`
  image's entrypoint checks the *host's* loaded modules (containers share the
  host kernel) and refuses to start otherwise.

All scripts live in `scripts/nfs-test-env/` and read shared config from
`scripts/nfs-test-env/env.sh`. Override via environment variables rather
than editing the scripts, e.g.:

```sh
NFS_SERVER_COUNT=4 ./scripts/nfs-test-env/01-setup-servers.sh
```

## Step 1 — stand up vanilla NFS servers

```sh
./scripts/nfs-test-env/01-setup-servers.sh
```

Creates a docker bridge network (`172.28.0.0/24` by default) and one
privileged container per server (`erichough/nfs-server`), each with a static
IP. Idempotent — re-run after changing `NFS_SERVER_COUNT` and it only adds
what's missing.

All servers export the **same** host directory (`$HOME/nfs-test-env-exports/root`
by default, no sudo needed — move it with `NFS_EXPORT_BASE`) with identical
`fsid=` values, which makes them behave as one crude cluster: a filehandle
issued by any node is valid on every other node, so `remoteports=` can
actually spread one mount's traffic across all of them. With independent
per-server directories — how this script used to work — spread I/O fails
with `ESTALE`. The evidence for that, and what this still doesn't simulate
(shared lock state, cache coherence), is in
[vastnfs-multipath-failover.md](vastnfs-multipath-failover.md).

Layout: `/export` (`fsid=0`, the NFSv4 pseudo-root), `/export/test`
(`fsid=1`), `/export/scratch` (`fsid=2`). Mind the path asymmetry — NFSv3
mounts `server:/export/scratch`, NFSv4 mounts `server:/scratch`.

Verified working end-to-end on Ubuntu 24.04/kernel 6.8.0-138-generic. One
thing to know if you restart the containers later (e.g. after a
`vastnfs-ctl reload`, see step 4a): if `nfsd` gets unloaded while a container
still holds it open, `docker stop` the containers first, reload, then
`sudo modprobe nfs` before `docker start`-ing them again — otherwise the
container's own healthcheck fails with "kernel module nfs is missing"
because it's checking the *host's* loaded modules.

## Step 2 — point xfstests at them

```sh
./scripts/nfs-test-env/02-configure-xfstests.sh
```

Writes `xfstests/local.config` with `FSTYP=nfs`, `TEST_DEV`/`TEST_DIR` on
server 1 and `SCRATCH_DEV`/`SCRATCH_MNT` on server 2 — the layout
`xfstests/README` and `common/rc`'s `_test_mount` expect.

## Step 3 — run xfstests against the inbox client

```sh
./scripts/nfs-test-env/03-run-xfstests.sh generic/001 generic/002
# or: ./scripts/nfs-test-env/03-run-xfstests.sh -g quick
```

This is your baseline: whatever kernel is currently loaded (inbox, at this
point) is what gets exercised. Record results before moving on — this is
what you diff against once VAST modules are in.

## Step 4 — build and install the VAST kernel modules

```sh
./scripts/nfs-test-env/04-build-install-vastnfs.sh
```

Runs `vastnfs-4.5.8/build.sh bin`, then installs the resulting `.deb`/`.rpm`
(on Ubuntu: `dist/vastnfs-modules_4.5.8-vastdata.kver.<uname -r>_amd64.deb`).
This is a host-wide kernel module replacement (see
`vastnfs-vs-linux.md`'s "Replacement, not coexistence").

### Step 4a — load the new modules: reboot, or reload live

The install alone doesn't load the new modules — the old ones stay resident
until something reloads them. Two ways to do that:

**Reboot** (what the script's final message suggests, and what
`vastnfs-4.5.8/docs/src/INSTALL.md` documents):

```sh
sudo reboot
```

**Or reload live, no reboot**, using the `vastnfs-ctl reload` command
documented in `vastnfs-4.5.8/docs/src/usage/vastnfs-ctl.md` — verified
working on the test VM:

```sh
# stop anything holding nfsd open first (e.g. the docker NFS servers,
# which use the host's shared nfsd module) -- otherwise reload fails with
# "rmmod: ERROR: Module nfsd is in use"
docker stop nfs-test-env-1 nfs-test-env-2 nfs-test-env-3

sudo vastnfs-ctl reload

# the reload only reloads modules that were in use; if nothing had `nfs`
# (the client module) mounted, it stays unloaded -- pull it back in so the
# next mount, and the docker servers' healthcheck, pick up the VAST build:
sudo modprobe nfs

docker start nfs-test-env-1 nfs-test-env-2 nfs-test-env-3
```

Confirm which one loaded with `modinfo nfs | grep filename` — VAST modules
live under `.../updates/bundle/...`, inbox ones under the normal kernel
module tree.

## Step 5 — verify the swap actually took

```sh
./scripts/nfs-test-env/05-verify-vastnfs.sh
```

Runs `vastnfs-ctl status` and cross-checks the loaded `sunrpc` module's
`srcversion` against the freshly built one, per
`vastnfs-4.5.8/docs/src/INSTALL.md`'s own verification method. If they
don't match, you haven't actually rebooted into the new modules yet.

## Step 6 — re-run the same xfstests

```sh
./scripts/nfs-test-env/03-run-xfstests.sh generic/001 generic/002
```

Same command as step 3, same servers, same `local.config` — the only thing
that changed is which kernel modules handle the client side. Diff the
results against step 3's.

## Step 7 — exercise VAST-specific multipath

`xfstests/tests/nfs/002` is an automated case for this (added after we
first confirmed no test covered `nconnect=`/`remoteports=`/`pconnect=` by
grepping `xfstests/tests` and `xfstests/common` for those strings). It
mounts scratch with `remoteports=<first-server-ip>-<last-server-ip>` across
all the docker servers from step 1, then counts the distinct `dstaddr=`
values reported by `vastnfs-ctl rpc-transports` — more than one is the
signal traffic is actually spread across multiple destination IPs (the VAST
addition) rather than opening N connections to a single address (the
upstream-only `nconnect` behavior). It `_notrun`s automatically on a
non-VAST client (via `/sys/module/sunrpc/parameters/nfs_bundle_version`) or
without a `NFS_MULTIPATH_REMOTEPORTS` range in `local.config` (written by
`02-configure-xfstests.sh`).

It proves spread only. It does **not** prove the mount survives losing a
node — measurements showing it does not are in
[vastnfs-multipath-failover.md](vastnfs-multipath-failover.md).

```sh
./scripts/nfs-test-env/03-run-xfstests.sh nfs/002
# or as part of a broader run: ./scripts/nfs-test-env/03-run-xfstests.sh -g vastnfs
```

`06-mount-multipath.sh` still exists for interactive/manual debugging
(mount by hand, eyeball `vastnfs-ctl` output) — `nfs/002` is what should
run in CI or a regression pass.

```sh
./scripts/nfs-test-env/06-mount-multipath.sh        # nconnect defaults to NFS_SERVER_COUNT
./scripts/nfs-test-env/06-mount-multipath.sh 8       # or pick an explicit nconnect
```

## Teardown

```sh
./scripts/nfs-test-env/99-teardown.sh
```

Removes the docker containers, network, and unmounts `TEST_MNT`/
`SCRATCH_MNT` if still mounted. Does **not** uninstall the VAST kernel
modules — that's a separate, host-wide step, see
`vastnfs-4.5.8/docs/src/UNINSTALL.md`.

## Known gaps / things to check as you iterate

- `04-build-install-vastnfs.sh` does not verify your kernel is in
  `vastnfs-4.5.8/docs/src/build/kernels.md`'s supported range before
  building — `build.sh` itself will refuse if it isn't, but you'll want to
  check that doc before spending time on the build.
- `nfs/002` only proves `remoteports=` spreads across destination
  addresses — it doesn't check `localports=`/`pconnect=` specifically, or
  compare against plain `nconnect=` staying on one address (a "contrast"
  test would strengthen the signal; see the plan this was built from,
  `nfs/003` is the natural next one if wanted).
- `nfs/001` in the `-g quick` group needs `TEST_DEV` mounted as NFSv4 and is
  skipped by default, since `02-configure-xfstests.sh` doesn't pin a
  version. Add `vers=4` to a manual mount, or extend the config, if you want
  it included.
- Verified end to end on the test VM: 644/644 `-g quick` tests passed with
  the inbox client (0 failures, rest correctly `[not run]` for NFS
  limitations — reflink/dedupe/quotas/dax/encryption/block-device-only
  checks). Re-run in progress against the VAST modules at time of writing;
  update this note with the comparison once it's done.
