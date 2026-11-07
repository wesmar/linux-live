#!/bin/bash
# 13-full-upgrade-and-clean.sh
# Performs apt full-upgrade on all base packages in the live image,
# purges caches, logs, autoclean, and rebuilds the squashfs image.
# Works in RAM (tmpfs), writes output image to OUT only.
set -uo pipefail

IMG=/mnt/c/debian-live/filesystem.squashfs
OUT="$1"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root

cleanup() {
  cd /
  for m in "$R/dev" "$R/proc" "$R/sys" "$B"; do
    mountpoint -q "$m" && umount -l "$m" 2>/dev/null || true
  done
  rmdir "$B" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }

mount -t tmpfs -o size=11g tmpfs "$B" || die "failed to mount tmpfs"
mkdir -p "$OUT"

echo "################ 1. Extract image"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die "unsquashfs failed"

echo "################ 2. Prepare chroot environment"
mount --bind /dev "$R/dev"
mount -t proc proc "$R/proc"
mount -t sysfs sysfs "$R/sys"

printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"
chmod 755 "$R/usr/sbin/policy-rc.d"

cp /etc/resolv.conf "$R/etc/resolv.conf.tmp"
cat > "$R/etc/resolv.conf" << 'EOF'
nameserver 1.1.1.1
nameserver 8.8.8.8
EOF

run_r() {
  chroot "$R" /usr/bin/env -i \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    LC_ALL=C HOME=/root \
    DEBIAN_FRONTEND=noninteractive \
    APT_LISTCHANGES_FRONTEND=none \
    INITRD=No "$@"
}

echo "################ 3. APT Update and Full Upgrade"
run_r apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -2

echo "Upgradable packages before:"
run_r apt list --upgradable 2>/dev/null | grep -v 'Listing...' || true

run_r apt-get -y -qq --no-install-recommends dist-upgrade > "$B/upgrade.log" 2>&1 || {
  tail -30 "$B/upgrade.log"
  die "apt-get dist-upgrade failed"
}
echo "Upgrade finished successfully."

# Autoremove and clean
run_r apt-get -y -qq --purge autoremove >/dev/null 2>&1 || true
run_r apt-get -qq clean
run_r apt-get -qq autoclean

# Verify no fuse or ntfs-3g packages
if run_r dpkg -l | grep -E 'ntfs-3g|fuse' ; then
  die "fuse or ntfs-3g was installed; aborting per requirement"
fi
echo "Verified: zero fuse / ntfs-3g packages installed."

# Ensure mount-win.sh has clean shutdown sync
sed -i '/if \[ -d \/run\/live\/medium \]; then/!b;n;c\      sync\n      mount -o remount,ro /run/live/medium 2>/dev/null || true' "$R/usr/local/bin/mount-win.sh" 2>/dev/null || true

echo "################ 4. Verify no remaining upgradable packages"
REMAINING=$(run_r apt list --upgradable 2>/dev/null | grep -vc 'Listing...' || true)
echo "Remaining upgradable packages: $REMAINING"
[ "$REMAINING" -eq 0 ] || die "packages still upgradable: $REMAINING"

echo "################ 5. Clean image and zero logs"
umount -l "$R/sys" "$R/proc" "$R/dev" 2>/dev/null || true
rm -f "$R/usr/sbin/policy-rc.d" "$R/etc/resolv.conf.tmp"

cat > "$R/etc/resolv.conf" << 'EOF'
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 1.0.0.1
EOF

find "$R" -xdev -type d -name __pycache__ -prune -exec rm -rf {} +
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/archives/*.deb "$R"/var/cache/apt/archives/partial/* "$R/root/.cache"
rm -f "$R"/var/cache/apt/*.bin "$R"/var/lib/dpkg/*-old "$R"/var/cache/debconf/*-old "$R"/var/log/apt/*.xz "$R/run/systemd/systemd-units-load" \
      "$R/etc/passwd-" "$R/etc/shadow-" "$R/etc/group-" "$R/etc/gshadow-" "$R/etc/.pwd.lock"
find "$R/var/log" -type f -exec truncate -s 0 {} +
find "$R/tmp" "$R/var/tmp" -mindepth 1 -delete 2>/dev/null || true

# Update timestamps for ldconfig / needsupdate checks
STAMP="$(stat -c %Y "$R/usr")000000000"
for f in etc/.updated var/.updated; do
  sed -i "s/^TIMESTAMP_NSEC=.*/TIMESTAMP_NSEC=$STAMP/" "$R/$f"
  touch -r "$R/usr" "$R/$f"
done

echo "################ 6. Pack squashfs"
START=$(date +%s)
mksquashfs "$R" "$OUT/filesystem.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || {
  tail -10 "$B/mk.log"
  die "mksquashfs failed"
}
echo "mksquashfs completed in $(( $(date +%s) - START ))s"
ls -l "$OUT/filesystem.squashfs" | awk '{print $5, $NF}'

rm -rf --one-file-system "$R"
echo "=== BUILD13 OK"
