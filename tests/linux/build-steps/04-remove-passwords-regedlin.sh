#!/bin/bash
# Removes the Wi-Fi credentials from the image and installs regedlin 0.2.2.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
cleanup() { cd /; mountpoint -q "$B" && umount "$B"; rmdir "$B" 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=9g tmpfs "$B" || die tmpfs
R=$B/root
C=etc/wpa_supplicant/wpa_supplicant.conf
NEWBIN="$1"
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"

sed -nE 's/^[[:space:]]*psk="?([^"]*)"?.*/\1/p' "$R/$C" | sort -u > "$B/psk"
echo "distinct credentials to look for: $(wc -l < "$B/psk")"
printf 'ctrl_interface=/run/wpa_supplicant\nupdate_config=1\ncountry=PL\n' > "$R/$C"
chmod 600 "$R/$C"

install -m 755 -o root -g root "$NEWBIN" "$R/usr/bin/regedlin" || die "install regedlin"
chroot "$R" /usr/bin/regedlin --help 2>&1 | head -1

W="$R/usr/local/bin/wifi-connect.sh"
python3 -I - "$W" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
replacements = (
    ("#    Otherwise the profiles shipped in the image stay in use.\n",
     "#    The image ships no Wi-Fi credentials of its own.\n"),
    ("    /usr/bin/regedlin wifi --sync /run/live/medium 2>/dev/null || true\n",
     "    /usr/bin/regedlin wifi --sync /run/live/medium >/dev/null 2>&1 || true\n"),
)
for old, new in replacements:
    if text.count(old) != 1:
        sys.exit(f"expected one match for: {old!r}")
    text = text.replace(old, new)
open(path, "w").write(text)
PY
sh -n "$W" || die "script syntax"
[ "$(grep -c 'regedlin' "$W")" = 1 ] || die "unexpected regedlin lines"
grep -qxF '    /usr/bin/regedlin wifi --sync /run/live/medium >/dev/null 2>&1 || true' "$W" || die "sync line differs"
sed -n '4,10p' "$W"

echo "--- other files containing a credential (paths only)"
grep -rlIF -f "$B/psk" "$R/etc" "$R/usr/local" "$R/root" "$R/var" 2>/dev/null | sed "s#$R##" | head -12
echo "(end of list)"

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
echo "--- metadata changes"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-90
echo "psk lines in the new image: $(grep -cE '^[[:space:]]*psk=' "$B/v/$C")"
stat -c '%A %U:%G %s' "$B/v/$C"
sha256sum "$B/v/usr/bin/regedlin" "$NEWBIN" | awk '{print substr($1,1,16)}' | uniq -c
rm -rf --one-file-system "$B/v"
rm -f "$B/psk"

cp "$B/new.squashfs" "$IMG" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "copy mismatch"
ls -l "$IMG" | awk '{print $5}'
sha256sum "$IMG" | cut -c1-64
echo "=== BUILD3 OK"
