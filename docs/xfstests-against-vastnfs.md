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
- Docker installed on that VM.
- `xfstests` built (`cd xfstests && make`) — see `xfstests/README`.
- `vastnfs-4.5.8/` and `linux/` checked out —
  `scripts/fetch-sources.sh` if not already done.
- Root/sudo access on the VM (module install, mount, docker --privileged).

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
privileged container per server (`erichough/nfs-server` by default — not
verified elsewhere in this repo, swap it via `NFS_SERVER_IMAGE` if it
doesn't suit your host), each with a static IP and its own export directory
under `/srv/nfs-test-env/<n>`. Idempotent — re-run after changing
`NFS_SERVER_COUNT` and it only adds what's missing.

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

Runs `vastnfs-4.5.8/build.sh bin`, then installs the resulting `.deb`/`.rpm`.
This is a host-wide kernel module replacement (see
`vastnfs-vs-linux.md`'s "Replacement, not coexistence") — **reboot after
this step**; the script stops short of doing that for you.

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

No xfstests case covers `nconnect=`/`remoteports=`/`pconnect=` — confirmed
by grepping `xfstests/tests` and `xfstests/common` for those strings, no
hits. `06-mount-multipath.sh` is the manual substitute:

```sh
./scripts/nfs-test-env/06-mount-multipath.sh        # nconnect defaults to NFS_SERVER_COUNT
./scripts/nfs-test-env/06-mount-multipath.sh 8       # or pick an explicit nconnect
```

Mounts with `remoteports=<first-server-ip>-<last-server-ip>` across all the
docker servers from step 1, then runs `vastnfs-ctl rpc-clients` and prints
what to look for: more than one distinct `remote_port_idx` across the
`xprt:` lines, which is the signal traffic is actually spread across
multiple destination IPs (the VAST addition) rather than opening N
connections to a single address (the upstream-only `nconnect` behavior).

## Teardown

```sh
./scripts/nfs-test-env/99-teardown.sh
```

Removes the docker containers, network, and unmounts `TEST_MNT`/
`SCRATCH_MNT` if still mounted. Does **not** uninstall the VAST kernel
modules — that's a separate, host-wide step, see
`vastnfs-4.5.8/docs/src/UNINSTALL.md`.

## Known gaps / things to check as you iterate

- `01-setup-servers.sh`'s choice of `erichough/nfs-server` is a reasonable
  default, not a verified one — if `nfsd` inside a privileged container
  doesn't behave the way you need on your host/kernel, swap it.
- `04-build-install-vastnfs.sh` does not verify your kernel is in
  `vastnfs-4.5.8/docs/src/build/kernels.md`'s supported range before
  building — `build.sh` itself will refuse if it isn't, but you'll want to
  check that doc before spending time on the build.
- Nothing here builds a corresponding xfstests test case for step 7's
  multipath check — it's a manual script, not a `tests/nfs/9xx`-style
  automated test. Worth writing one if this becomes a repeated check rather
  than an occasional one.
