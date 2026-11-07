#!/bin/bash
# 12-install-tools-and-mount-win.sh
# Installs low-level system tools (efibootmgr, mokutil, cryptsetup, nvme-cli, parted, gdisk,
# msr-tools, tcpdump, dosfstools) without fuse/ntfs-3g, and configures rw automount for /mnt/win.
# Runs in tmpfs, writes output image to OUT only.
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

# Prevent daemons from auto-starting during apt install
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"
chmod 755 "$R/usr/sbin/policy-rc.d"

# Temporary DNS for apt download inside chroot
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

echo "################ 3. Install low-level tools via APT"
run_r apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -2

PACKAGES=(
  efibootmgr
  mokutil
  cryptsetup
  nvme-cli
  parted
  gdisk
  msr-tools
  tcpdump
  dosfstools
)

run_r apt-get -y -qq --no-install-recommends install "${PACKAGES[@]}" > "$B/apt.log" 2>&1 || {
  tail -20 "$B/apt.log"
  die "apt-get install failed"
}

# Verify no fuse or ntfs-3g was pulled in
if run_r dpkg -l | grep -E 'ntfs-3g|fuse' ; then
  die "fuse or ntfs-3g was installed; aborting per requirement"
fi
echo "Verified: zero fuse / ntfs-3g packages installed."

# Mask smartmontools daemon so it does not delay boot
run_r systemctl mask smartmontools.service smartd.service 2>/dev/null || true

echo "################ 4. Configure /mnt/win rw automount"
mkdir -p "$R/mnt/win"

cat > "$R/usr/local/bin/mount-win.sh" << 'EOF'
#!/bin/bash
# Mounts Windows C: read-write under /mnt/win with kernel NTFSPlus driver
set -euo pipefail

OPTS="rw,noatime,nocase,windows_names,iocharset=utf8"
TARGET="/mnt/win"
mkdir -p "$TARGET"

case "${1:-start}" in
  start)
    if mountpoint -q "$TARGET"; then
      exit 0
    fi

    # Case 1: BitLocker encrypted volume (active from initrd unlock-bitlocker)
    if dmsetup info bitlocker0 >/dev/null 2>&1; then
      if ! dmsetup info bitlocker_rw >/dev/null 2>&1; then
        dmsetup table --showkeys bitlocker0 | dmsetup create bitlocker_rw
      fi
      for i in $(seq 1 10); do
        [ -b /dev/mapper/bitlocker_rw ] && break
        sleep 0.1
      done
      mount -t ntfs -o "$OPTS" /dev/mapper/bitlocker_rw "$TARGET"
      exit 0
    fi

    # Case 2: Non-BitLocker volume (mounted ro under /run/live/medium)
    MEDIUM_DEV=$(findmnt -no SOURCE /run/live/medium 2>/dev/null || true)
    if [ -n "$MEDIUM_DEV" ] && [ -b "$MEDIUM_DEV" ]; then
      mount -o remount,rw /run/live/medium 2>/dev/null || true
      if ! mount -t ntfs -o "$OPTS" "$MEDIUM_DEV" "$TARGET" 2>/dev/null; then
        mount --bind /run/live/medium "$TARGET"
      fi
      exit 0
    fi
    ;;

  stop)
    if mountpoint -q "$TARGET"; then
      umount -l "$TARGET" 2>/dev/null || true
    fi
    if dmsetup info bitlocker_rw >/dev/null 2>&1; then
      dmsetup remove bitlocker_rw 2>/dev/null || true
    fi
    ;;

  status)
    findmnt "$TARGET" || true
    ;;

  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
EOF
chmod 755 "$R/usr/local/bin/mount-win.sh"

cat > "$R/etc/systemd/system/mount-win.service" << 'EOF'
[Unit]
Description=Mount Windows C: read-write at /mnt/win
DefaultDependencies=no
After=run-live-medium.mount
Before=shutdown.target umount.target
Conflicts=shutdown.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/mount-win.sh start
ExecStop=/usr/local/bin/mount-win.sh stop
TimeoutSec=10

[Install]
WantedBy=multi-user.target
EOF

# Enable service
mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/mount-win.service "$R/etc/systemd/system/multi-user.target.wants/mount-win.service"

# Add LazyUnmount override for /mnt/win mount unit
mkdir -p "$R/etc/systemd/system/mnt-win.mount.d"
cat > "$R/etc/systemd/system/mnt-win.mount.d/override.conf" << 'EOF'
[Mount]
LazyUnmount=true
EOF

echo "################ 5. Verify installed binaries"
run_r efibootmgr --version || die "efibootmgr check failed"
run_r mokutil --version 2>&1 | head -1 || die "mokutil check failed"
run_r cryptsetup --version || die "cryptsetup check failed"
run_r nvme version || die "nvme check failed"
run_r parted --version | head -1 || die "parted check failed"
run_r tcpdump --version | head -1 || die "tcpdump check failed"

echo "################ 6. Clean image"
umount -l "$R/sys" "$R/proc" "$R/dev" 2>/dev/null || true
rm -f "$R/usr/sbin/policy-rc.d" "$R/etc/resolv.conf.tmp"

# Restore static resolv.conf
cat > "$R/etc/resolv.conf" << 'EOF'
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 1.0.0.1
EOF

# Clean apt caches, old files, logs
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

echo "################ 7. Pack squashfs"
START=$(date +%s)
mksquashfs "$R" "$OUT/filesystem.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || {
  tail -10 "$B/mk.log"
  die "mksquashfs failed"
}
echo "mksquashfs completed in $(( $(date +%s) - START ))s"
ls -l "$OUT/filesystem.squashfs" | awk '{print $5, $NF}'

rm -rf --one-file-system "$R"
echo "=== BUILD12 OK"
