#!/bin/bash
# 16-clean-ntfs-shutdown.sh
# The Windows volume is mounted read-write at /mnt/win (kernel NTFS driver). It has to be unmounted
# cleanly at shutdown: only a real unmount lets the driver mark the volume clean again. A volume left
# "dirty" makes Windows ask for a disk check and refuse to shrink or resize it.
#
# Before: mount-win.sh stop used "umount -l", and mnt-win.mount had LazyUnmount=true. A lazy unmount
# takes the mount out of sight before systemd-shutdown can remount it read-only, so the volume stayed
# dirty after every session that wrote to it. Without BitLocker it was worse: /run/live/medium itself
# was remounted read-write and never went back.
# After:
#   * mount-win.sh stop: sync, a real umount (retried; processes still using /mnt/win are ended),
#     remount read-only as the fallback, "umount -l" only as the very last resort; the read-write
#     BitLocker mapping is removed afterwards; without BitLocker /run/live/medium goes back to ro;
#   * wll-persistence.service stops before mount-win.service (its auto-save writes to /mnt/win);
#   * mnt-win.mount loses LazyUnmount=true.
# esp.tar is not touched. Works only in a private tmpfs that the trap removes; writes
# filesystem.squashfs and its .sha256 to OUT and nothing else.
#
#   16-clean-ntfs-shutdown.sh IMAGE OUT     e.g.  16-clean-ntfs-shutdown.sh /mnt/c/Temp/linux-live/filesystem.squashfs /mnt/c/Temp/linux-live-16
set -uo pipefail
IMG="${1:?usage: $0 IMAGE OUT}"
OUT="${2:?usage: $0 IMAGE OUT}"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root
cleanup() {
  cd /
  mountpoint -q "$B" && umount "$B"
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
[ "$(realpath "$OUT")" != "$(realpath "$(dirname "$IMG")")" ] || die "OUT must not be the folder of IMAGE"
mount -t tmpfs -o size=8g tmpfs "$B" || die tmpfs
mkdir -p "$OUT"
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }
hash() { ( cd "$1" && find . -type f -print0 | xargs -0 -P8 -n200 sha256sum | LC_ALL=C sort -k2 ); }

echo "################ 1. extract"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || { tail -3 "$B/x.log"; die unsquashfs; }
tail -1 "$B/x.log"; rm -f "$B/x.log"
meta "$R" > "$B/meta.before"; hash "$R" > "$B/sha.before"
[ -x "$R/bin/fuser" ] || die "fuser (psmisc) missing in the image"

echo "################ 2. mount-win.sh: clean stop"
python3 -I - "$R/usr/local/bin/mount-win.sh" <<'PY' || die "mount-win.sh update"
import sys
path = sys.argv[1]
text = open(path, encoding='utf-8').read()
old = '''  stop)
    if mountpoint -q "$TARGET"; then
      umount -l "$TARGET" 2>/dev/null || true
    fi
    if dmsetup info bitlocker_rw >/dev/null 2>&1; then
      dmsetup remove bitlocker_rw 2>/dev/null || true
    fi
    ;;
'''
new = '''  stop)
    # Only a real unmount lets the driver mark the volume clean; a volume left dirty makes Windows
    # ask for a disk check and refuse to shrink it. "umount -l" is the very last resort.
    if mountpoint -q "$TARGET"; then
      sync
      for attempt in 1 2 3 4 5 6; do
        umount "$TARGET" 2>/dev/null && break
        [ "$attempt" -ge 3 ] && fuser -km "$TARGET" >/dev/null 2>&1
        sleep 0.5
      done
      if mountpoint -q "$TARGET"; then
        mount -o remount,ro "$TARGET" 2>/dev/null || true
        umount -l "$TARGET" 2>/dev/null || true
      fi
    fi
    # The read-write BitLocker mapping goes once nothing uses it any more.
    for attempt in 1 2 3 4 5; do
      dmsetup info bitlocker_rw >/dev/null 2>&1 || break
      dmsetup remove bitlocker_rw 2>/dev/null && break
      sleep 0.2
    done
    # Without BitLocker /run/live/medium was made read-write for /mnt/win: back to read-only, which
    # marks the volume clean before the live system lets go of it.
    if ! dmsetup info bitlocker0 >/dev/null 2>&1 && mountpoint -q /run/live/medium; then
      sync
      mount -o remount,ro /run/live/medium 2>/dev/null || true
    fi
    ;;
'''
if text.count(old) != 1:
    sys.exit('stop section not found exactly once')
open(path, 'w', encoding='utf-8').write(text.replace(old, new))
PY
bash -n "$R/usr/local/bin/mount-win.sh" || die "mount-win.sh syntax"

echo "################ 3. units"
python3 -I - "$R/etc/systemd/system" <<'PY' || die "unit update"
import sys
base = sys.argv[1]

def edit(name, old, new):
    p = f'{base}/{name}'
    s = open(p, encoding='utf-8').read()
    if s.count(old) != 1:
        sys.exit(f'{name}: {old!r} not found exactly once')
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))

# Units stop in the reverse order of After=: the auto-save finishes before /mnt/win goes away.
edit('wll-persistence.service', 'DefaultDependencies=no\n', 'DefaultDependencies=no\nAfter=mount-win.service\nWants=mount-win.service\n')
# A clean unmount may need longer than 10 s when the auto-save has just written a large file.
edit('mount-win.service', 'TimeoutSec=10\n', 'TimeoutStartSec=10\nTimeoutStopSec=30\n')
PY
rm -f "$R/etc/systemd/system/mnt-win.mount.d/override.conf"
rmdir "$R/etc/systemd/system/mnt-win.mount.d" 2>/dev/null
systemd-analyze --root="$R" verify mount-win.service wll-persistence.service 2>&1 | grep -E 'mount-win|wll-persistence' | head -5
grep -n -A1 '^DefaultDependencies' "$R/etc/systemd/system/wll-persistence.service"

echo "################ 4. pack and verify"
meta "$R" > "$B/meta.after"; hash "$R" > "$B/sha.after"
echo "image changes (metadata):"; diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | awk '{print "  " $1 " " $7}' | sort -u
echo "image changes (content):";  diff "$B/sha.before" "$B/sha.after" | grep -E '^[<>]' | awk '{print "  " $3}' | sort -u
START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf "$R" "$B/mk.log"
unsquashfs -s "$B/new.squashfs" 2>/dev/null | grep -E 'Compression|Block size|Number of inodes'
unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > /dev/null 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.check"
diff -q "$B/meta.after" "$B/meta.check" > /dev/null || die "packed image differs from the tree"
rm -rf "$B/v"

cp "$B/new.squashfs" "$OUT/filesystem.squashfs.part" || die "copy"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$OUT/filesystem.squashfs.part")" ] || die "copy mismatch"
mv -f "$OUT/filesystem.squashfs.part" "$OUT/filesystem.squashfs"
( cd "$OUT" && sha256sum filesystem.squashfs > filesystem.squashfs.sha256 && cat filesystem.squashfs.sha256 )
echo "=== BUILD OK"
