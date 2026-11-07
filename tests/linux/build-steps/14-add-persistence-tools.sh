#!/bin/bash
# 14-add-persistence-tools.sh
# Installs squashfs-tools (mksquashfs) and deploys wll-commit persistence suite.
# Enables delta persistence saving to /mnt/win/debian-live/persistence.squashfs.
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
nameserver 1.0.0.1
EOF

run_r() {
  chroot "$R" /usr/bin/env -i \
    PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    LC_ALL=C HOME=/root \
    DEBIAN_FRONTEND=noninteractive \
    APT_LISTCHANGES_FRONTEND=none \
    INITRD=No "$@"
}

echo "################ 3. Install squashfs-tools"
run_r apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -2
run_r apt-get -y -qq --no-install-recommends install squashfs-tools > "$B/apt.log" 2>&1 || {
  tail -20 "$B/apt.log"
  die "failed to install squashfs-tools"
}
run_r mksquashfs -version | head -1

echo "################ 4. Deploy wll-commit persistence tool"
cat > "$R/usr/local/bin/wll-commit" << 'EOF'
#!/bin/bash
# wll-commit: Commits live session modifications to /mnt/win/debian-live/persistence.squashfs
set -euo pipefail

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

CONFIG="/etc/default/wll-persistence"
[ -f "$CONFIG" ] && . "$CONFIG"
AUTO_SAVE="${AUTO_SAVE_ON_SHUTDOWN:-0}"

ACTION="${1:-commit}"

case "$ACTION" in
  --shutdown)
    if [ "$AUTO_SAVE" != "1" ]; then
      exit 0
    fi
    echo "[wll-commit] Auto-saving session before shutdown..."
    ;;

  --status|-s)
    echo "=== WinLinLive Persistence Status ==="
    WIN_DIR="/mnt/win/debian-live"
    PERSIST_FILE="$WIN_DIR/persistence.squashfs"
    if [ -f "$PERSIST_FILE" ]; then
      SIZE=$(stat -c %s "$PERSIST_FILE" 2>/dev/null || stat -f %z "$PERSIST_FILE" 2>/dev/null)
      SIZE_HR=$(numfmt --to=iec-i --suffix=B "$SIZE" 2>/dev/null || echo "$SIZE bytes")
      echo "Persistence file: $PERSIST_FILE ($SIZE_HR)"
    else
      echo "Persistence file: Not present (clean baseline mode)"
    fi
    if [ -d "/run/live/rootfs/persistence.squashfs" ]; then
      echo "Active layer: Loaded at boot (/run/live/rootfs/persistence.squashfs)"
    else
      echo "Active layer: None loaded at boot"
    fi
    RW_DIR="/run/live/overlay/rw"
    if [ -d "$RW_DIR" ]; then
      CHANGED_COUNT=$(find "$RW_DIR" -mindepth 1 2>/dev/null | wc -l)
      echo "Session buffer (RAM): $CHANGED_COUNT items modified/created"
    fi
    echo "Auto-save on shutdown: $AUTO_SAVE"
    exit 0
    ;;

  --auto-on)
    sed -i 's/^AUTO_SAVE_ON_SHUTDOWN=.*/AUTO_SAVE_ON_SHUTDOWN=1/' "$CONFIG" 2>/dev/null || \
      echo "AUTO_SAVE_ON_SHUTDOWN=1" >> "$CONFIG"
    echo "Auto-save on shutdown: ENABLED"
    exit 0
    ;;

  --auto-off)
    sed -i 's/^AUTO_SAVE_ON_SHUTDOWN=.*/AUTO_SAVE_ON_SHUTDOWN=0/' "$CONFIG" 2>/dev/null || \
      echo "AUTO_SAVE_ON_SHUTDOWN=0" >> "$CONFIG"
    echo "Auto-save on shutdown: DISABLED"
    exit 0
    ;;

  --reset)
    WIN_DIR="/mnt/win/debian-live"
    PERSIST_FILE="$WIN_DIR/persistence.squashfs"
    if ! mountpoint -q /mnt/win; then
      /usr/local/bin/mount-win.sh start >/dev/null 2>&1 || true
    fi
    if [ -f "$PERSIST_FILE" ]; then
      rm -f "$PERSIST_FILE"
      echo "[wll-commit] Persistence file removed."
      echo "Next boot will start in clean factory baseline state."
    else
      echo "[wll-commit] Persistence file does not exist. Already in baseline state."
    fi
    exit 0
    ;;

  commit|save|"")
    ;;

  *)
    echo "Usage: $0 [commit|--status|--auto-on|--auto-off|--reset]"
    exit 1
    ;;
esac

history -a 2>/dev/null || true

# 1. Ensure Windows C: is mounted read-write
if ! mountpoint -q /mnt/win; then
  echo "[wll-commit] Mounting /mnt/win..."
  /usr/local/bin/mount-win.sh start
fi

WIN_DIR="/mnt/win/debian-live"
if [ ! -d "$WIN_DIR" ]; then
  echo "FATAL: $WIN_DIR does not exist on /mnt/win" >&2
  exit 1
fi

RW_DIR="/run/live/overlay/rw"
if [ ! -d "$RW_DIR" ]; then
  echo "FATAL: $RW_DIR not found. Is this a live-boot session?" >&2
  exit 1
fi

TARGET="$WIN_DIR/persistence.squashfs"
TMP_TARGET="$WIN_DIR/persistence.squashfs.new.$$"
PREV_PERSIST="/run/live/rootfs/persistence.squashfs"

START_TS=$(date +%s%N 2>/dev/null || date +%s)
echo "[wll-commit] Preparing delta layer..."

EXCLUDES=(
  -e "tmp/*"
  -e "var/tmp/*"
  -e "var/log/*"
  -e "var/cache/apt/archives/*.deb"
  -e "run/*"
  -e "mnt/*"
  -e "media/*"
)

STAGE=""
if [ -d "$PREV_PERSIST" ]; then
  echo "[wll-commit] Merging previous persistence layer with session modifications..."
  STAGE=$(mktemp -d /run/wll-stage.XXXXXX)
  cleanup_stage() { rm -rf "$STAGE"; }
  trap cleanup_stage EXIT INT TERM HUP

  cp -a "$PREV_PERSIST/." "$STAGE/"
  cp -a "$RW_DIR/." "$STAGE/"
  SRC="$STAGE"
else
  SRC="$RW_DIR"
fi

echo "[wll-commit] Compressing with zstd (level 15)..."
mksquashfs "$SRC" "$TMP_TARGET" \
  -comp zstd \
  -Xcompression-level 15 \
  -b 1024K \
  -noappend \
  -wildcards \
  "${EXCLUDES[@]}" >/dev/null

sync "$WIN_DIR"
mv -f "$TMP_TARGET" "$TARGET"
sync "$TARGET"

if [ -n "$STAGE" ] && [ -d "$STAGE" ]; then
  rm -rf "$STAGE"
  trap - EXIT INT TERM HUP
fi

END_TS=$(date +%s%N 2>/dev/null || date +%s)
DURATION_MS=$(( (END_TS - START_TS) / 1000000 )) 2>/dev/null || DURATION_MS=0
FINAL_SZ=$(stat -c %s "$TARGET" 2>/dev/null || stat -f %z "$TARGET" 2>/dev/null)
FINAL_SZ_HR=$(numfmt --to=iec-i --suffix=B "$FINAL_SZ" 2>/dev/null || echo "$FINAL_SZ bytes")

echo "[wll-commit] OK: $TARGET updated ($FINAL_SZ_HR) in ${DURATION_MS}ms."
echo "[wll-commit] Changes are persistent across reboots."
EOF
chmod 755 "$R/usr/local/bin/wll-commit"
ln -sf wll-commit "$R/usr/local/bin/save-session"

echo "################ 5. Deploy configuration and systemd service"
mkdir -p "$R/etc/default"
cat > "$R/etc/default/wll-persistence" << 'EOF'
# WinLinLive Persistence Configuration
# Set to 1 to automatically save session modifications on shutdown/reboot.
# Set to 0 to only save when explicitly running 'wll-commit' or 'save-session'.
AUTO_SAVE_ON_SHUTDOWN=0
EOF

cat > "$R/etc/systemd/system/wll-persistence.service" << 'EOF'
[Unit]
Description=WinLinLive Session Persistence Auto-Save
DefaultDependencies=no
Before=umount.target shutdown.target
Conflicts=umount.target

[Service]
Type=oneshot
ExecStop=/usr/local/bin/wll-commit --shutdown
RemainAfterExit=yes
TimeoutStopSec=30

[Install]
WantedBy=basic.target
EOF

run_r systemctl enable wll-persistence.service

echo "################ 6. Clean image and verify zero packages to upgrade"
run_r apt-get -y -qq --purge autoremove >/dev/null 2>&1 || true
run_r apt-get -qq clean
run_r apt-get -qq autoclean

if run_r dpkg -l | grep -E 'ntfs-3g|fuse' ; then
  die "fuse or ntfs-3g was installed; aborting per requirement"
fi
echo "Verified: zero fuse / ntfs-3g packages installed."

REMAINING=$(run_r apt list --upgradable 2>/dev/null | grep -vc 'Listing...' || true)
echo "Remaining upgradable packages: $REMAINING"
[ "$REMAINING" -eq 0 ] || die "packages still upgradable: $REMAINING"

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
echo "=== BUILD14 OK"
