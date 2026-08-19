#!/bin/bash
# Self-check for the xfs loop-image backing store that
# 00-run-xfstests-on-gh-ci.sh builds its exports on.
#
# It asserts the two capacities that motivated the change, because both are
# invisible to xfstests over NFS -- FSTYP is "nfs" and there is no probe for
# the server's backing filesystem, so tests/generic/020 asserts
# max_attrs=1000 and generic/486 asserts a 65536-byte xattr value on faith.
# ext4 with 4k blocks gives 112 and one block respectively, which is what
# made both fail in CI. If this check passes, an ext4 regression in the
# backing store cannot reach xfstests unnoticed.
#
# It also covers the traps in the setup that are easy to get wrong: the chmod
# has to follow the mount, umount has to detach the loop device by itself,
# and truncate rejects the fractional sizes that numfmt --to=iec produces.
#
# Needs root, mkfs.xfs and a loop device. Does not need NFS.
#
# Usage: sudo bash scripts/check-xfs-backing-store.sh

set -euo pipefail

IMG_SIZE_BYTES="${IMG_SIZE_BYTES:-1073741824}"   # 1GiB, in bytes on purpose

work="$(mktemp -d)"
img="${work}/scratch.img"
mnt="${work}/mnt"
failures=0

cleanup() {
    mountpoint -q "$mnt" 2>/dev/null && umount "$mnt" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

ok()   { printf 'ok       %s\n' "$*"; }
fail() { printf 'NOT OK   %s\n' "$*"; failures=$((failures + 1)); }

command -v mkfs.xfs >/dev/null || { echo "mkfs.xfs not found -- install xfsprogs" >&2; exit 2; }
command -v attr     >/dev/null || { echo "attr not found -- install attr" >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 2; }

mkdir -p "$mnt"
truncate -s "$IMG_SIZE_BYTES" "$img"
mkfs.xfs -q "$img"
mount -o loop "$img" "$mnt"
chmod 777 "$mnt"

[ "$(findmnt -no FSTYPE "$mnt")" = "xfs" ] &&
    ok "the export root is xfs" ||
    fail "the export root is $(findmnt -no FSTYPE "$mnt"), not xfs"

# The chmod-after-mount ordering. Applied before the mount it lands on the
# hidden directory underneath and xfstests' chowns fail against a 0755 root.
[ "$(stat -c %a "$mnt")" = "777" ] &&
    ok "the mounted export root is world-writable" ||
    fail "the mounted export root is $(stat -c %a "$mnt"), not 777"

# generic/020: _attr_get_max asserts 1000 for FSTYP=nfs.
f="${mnt}/attrs"; : > "$f"
n=0
while [ "$n" -lt 1000 ]; do
    setfattr -n "user.attribute_${n}" -v "value_${n}" "$f" 2>/dev/null || break
    n=$((n + 1))
done
[ "$n" -ge 1000 ] &&
    ok "1000 xattrs on one inode (generic/020 needs 1000)" ||
    fail "only $n xattrs fit on one inode -- generic/020 needs 1000"

# generic/486: attr_replace_test -m 65536. The value goes in on stdin, not
# in -v: a single argv entry is capped at 128kB and the hex form of 65536
# bytes is 131074 characters, so setfattr -v fails with E2BIG before xfs is
# ever asked.
g="${mnt}/bigval"; : > "$g"
val="${work}/val"
head -c 65536 /dev/zero | tr '\0' 'x' > "$val"
if attr -q -s world "$g" < "$val" >/dev/null 2>&1; then
    ok "65536-byte xattr value (generic/486 needs 65536)"
else
    fail "a 65536-byte xattr value was rejected -- generic/486 needs it"
fi

# generic/103 fallocates all free space bar ~512kB. The point of the image is
# that this is bounded: the fill must stay inside the image and leave the
# host filesystem alone.
img_avail="$(df -B1 --output=avail "$mnt" | tail -1)"
fallocate -l "$((img_avail - 512 * 1024))" "${mnt}/spc" 2>/dev/null || true
if [ "$(df -B1 --output=avail "$mnt" | tail -1)" -lt "$((img_avail / 2))" ]; then
    ok "the export filesystem can be filled (generic/103's premise)"
else
    fail "could not fill the export filesystem -- generic/103 will not behave"
fi
# The image is sparse, so filling it does consume host space, but only up to
# the image size. What must not happen is the host running dry.
if [ "$(df -B1 --output=avail "$work" | tail -1)" -gt 0 ]; then
    ok "filling the export left the host filesystem with space"
else
    fail "filling the export exhausted the host filesystem"
fi
rm -f "${mnt}/spc"

# The auto-sizing in 00-run-xfstests-on-gh-ci.sh divides available space and
# must hand truncate a byte count. numfmt --to=iec emits things like "4.5G",
# which truncate rejects as an invalid number -- so the script keeps bytes as
# its currency and this pins that down.
if truncate -s "$(numfmt --to=iec 4831838208)" "${work}/frac.img" 2>/dev/null; then
    fail "truncate accepted a fractional suffix -- the bytes-only rule may be stale"
else
    ok "truncate rejects fractional suffixes, so sizes must stay in bytes"
fi
rm -f "${work}/frac.img"

# umount must detach the loop device on its own, or teardown leaks one per
# run until the box is out of loop devices.
loopdev="$(losetup -j "$img" | cut -d: -f1)"
umount "$mnt"
if [ -n "$loopdev" ] && [ -z "$(losetup -j "$img" 2>/dev/null)" ]; then
    ok "umount detached the loop device ($loopdev)"
else
    fail "the loop device for $img is still attached after umount"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all checks passed"
else
    echo "$failures check(s) failed"
fi
exit $((failures > 0))
