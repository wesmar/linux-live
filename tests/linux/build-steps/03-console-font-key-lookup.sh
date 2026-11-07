#!/bin/bash
# Second pass on the .new files: resolution-aware console font, key lookup on any FAT partition.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
INITRD=/mnt/c/debian-live/initrd.img.new
HELPER="$1"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
cleanup() { cd /; mountpoint -q "$B" && umount "$B"; rmdir "$B" 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=9g tmpfs "$B" || die tmpfs
R=$B/root
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

echo "################ 1. initrd: look for the key on every FAT partition"
python3 -I "$HELPER" "$INITRD" "$B" save || die split
mkdir -p "$B/c"
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
python3 -I - "$B/c/usr/sbin/unlock-bitlocker" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
start = text.index("boot=$(blkid -t LABEL=DEBIANLIVE")
end = text.index("index=0\n")
new = """mkdir -p /run/debianlive
for part in $(blkid -t TYPE=vfat -o device 2>/dev/null); do
    mount -t vfat -o ro "$part" /run/debianlive 2>/dev/null || continue
    cp /run/debianlive/debian-live/bitlocker.key "$key" 2>/dev/null
    umount /run/debianlive
    [ -s "$key" ] && break
done
rmdir /run/debianlive

"""
text = text[:start] + new + text[end:]
text = text.replace("bitlocker.key on the DEBIANLIVE partition", "debian-live/bitlocker.key on a FAT partition")
open(path, "w").write(text)
PY
sh -n "$B/c/usr/sbin/unlock-bitlocker" || die "script syntax"
sed -n '1,4p;16,26p' "$B/c/usr/sbin/unlock-bitlocker"
( cd "$B/c" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o | zstd -19 -T0 -q > "$B/new_c.zst" )
python3 -I - "$B" <<'PY'
import sys
base = sys.argv[1]
part_a = open(base + "/parta.cpio", "rb").read()
part_a += b"\0" * ((512 - len(part_a) % 512) % 512)
part_b = open(base + "/partb.cpio", "rb").read()
part_b += b"\0" * ((512 - len(part_b) % 512) % 512)
with open(base + "/new_initrd", "wb") as out:
    out.write(part_a)
    out.write(part_b)
    out.write(open(base + "/new_c.zst", "rb").read())
PY
mkdir -p "$B/chk"
python3 -I "$HELPER" "$B/new_initrd" "$B/chk" save || die structure
zstd -tq "$B/chk/partc.zst" || die "zstd test"
zstd -dcq "$B/chk/partc.zst" | cpio -it --quiet 2>/dev/null | grep -cE '^usr/sbin/(cryptsetup|unlock-bitlocker)$'
cmp "$B/partb.cpio" <(head -c "$(stat -c %s "$B/partb.cpio")" "$B/chk/partb.cpio") && echo "part B unchanged"
cp "$B/new_initrd" "$INITRD" || die "copy initrd"
ls -l "$INITRD" | awk '{print $5}'
rm -rf "$B/c" "$B/chk" "$B"/*.cpio "$B"/*.zst "$B/new_initrd"

echo "################ 2. image: console font"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"
cat > "$R/usr/local/bin/console-font.sh" <<'EOF'
#!/bin/sh
# console-font.sh - Use a large console font on high resolution screens.
width=$(cut -d, -f1 /sys/class/graphics/fb0/virtual_size 2>/dev/null)
[ -n "$width" ] && [ "$width" -ge 2560 ] || exit 0

for tty in /dev/tty[1-6]; do
    /usr/bin/setfont -C "$tty" Lat2-TerminusBold32x16 2>/dev/null
done
exit 0
EOF
cat > "$R/etc/systemd/system/console-font.service" <<'EOF'
[Unit]
Description=Select console font for the screen resolution
After=console-setup.service systemd-vconsole-setup.service

[Service]
Type=oneshot
ExecStart=/usr/local/bin/console-font.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
chmod 755 "$R/usr/local/bin/console-font.sh"
chmod 644 "$R/etc/systemd/system/console-font.service"
ln -sf /etc/systemd/system/console-font.service "$R/etc/systemd/system/multi-user.target.wants/console-font.service"
sh -n "$R/usr/local/bin/console-font.sh" || die "font script syntax"
systemd-analyze --root="$R" verify console-font.service 2>&1 | grep -E 'console-font' | head -4

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

echo "################ 3. verify"
unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-105
cp "$B/new.squashfs" "$IMG" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "copy mismatch"
ls -l "$IMG" | awk '{print $5}'
sha256sum "$IMG" "$INITRD" | sed 's#/mnt/c/debian-live/##'
echo "=== BUILD2 OK"
