#!/bin/bash
# 15-layout-from-grub.sh
# No folder name is written into the system any more; grub.cfg is the only place that holds one.
#   ESP folder   : $prefix of the signed GRUB (Debian: /EFI/debian) - fixed by the signature, not chosen here.
#   media folder : "set media=..." in grub.cfg (default linux-live) - passed to the kernel as live-media-path;
#                  unlock-bitlocker, ssh-import.sh and wll-commit read it from /proc/cmdline.
# LinuxLive.exe reads both from esp.tar, so a package decides its own layout.
#
# The signed files (shimx64.efi, grubx64.efi, vmlinuz) and the initrd parts with microcode and
# modules are carried over byte for byte; the step fails if any of them changes.
# Works only in a private tmpfs that the trap removes; writes esp.tar, filesystem.squashfs and
# their .sha256 files to OUT and nothing else.
#
#   15-layout-from-grub.sh OUT [MEDIA]      e.g.  15-layout-from-grub.sh /mnt/c/Temp/linux-live linux-live
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs
PKG=/mnt/c/debian-live/esp.tar
OUT="${1:?usage: $0 OUT [MEDIA]}"
MEDIA="${2:-linux-live}"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root
cleanup() {
  cd /
  mountpoint -q "$B" && umount "$B"
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
case "$MEDIA" in *[!A-Za-z0-9._-]*|""|.*) die "media folder must be a plain name: $MEDIA";; esac
mount -t tmpfs -o size=8g tmpfs "$B" || die tmpfs
mkdir -p "$OUT" "$B/esp" "$B/c" "$B/chk"
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }
hash() { ( cd "$1" && find . -type f -print0 | xargs -0 -P8 -n200 sha256sum | LC_ALL=C sort -k2 ); }
signed="shimx64.efi grubx64.efi vmlinuz unicode.pf2"

echo "################ 1. ESP package"
tar -xf "$PKG" -C "$B/esp" || die "esp.tar"
( cd "$B/esp" && sha256sum $signed ) > "$B/signed.before" || die "signed files missing"
PREFIX=$(python3 -I - "$B/esp/grubx64.efi" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
count, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
for i in range(count):
    name, _, _, size, ptr = struct.unpack_from('<8sIIII', d, pe + 24 + opt + i * 40)
    if name.rstrip(b'\0') != b'mods':
        continue
    _, _, off, total = struct.unpack_from('<IIQQ', d, ptr)
    pos, end = ptr + off, ptr + total
    while pos + 8 <= end:
        kind, length = struct.unpack_from('<II', d, pos)
        if length < 8:
            break
        if kind == 3:  # OBJ_TYPE_PREFIX
            print(d[pos + 8:pos + length].rstrip(b'\0').decode())
            sys.exit(0)
        pos += length
sys.exit(1)
PY
) || die "no prefix in grubx64.efi"
echo "signed GRUB prefix: $PREFIX (the ESP folder)"
echo "media folder      : $MEDIA"

echo "################ 2. grub.cfg"
cat > "$B/esp/grub.cfg" <<EOF
set timeout=5
set default=0

# Folder of the live system on the Windows volume. The only name in the system:
# LinuxLive.exe reads it here, the kernel gets it as live-media-path, the scripts in the
# image read it from /proc/cmdline.
set media=$MEDIA

# The signed GRUB runs from the ESP folder it was built for (\$prefix). A small ESP may keep
# the kernel and initrd on another FAT partition instead, in /\$media.
set live=\$prefix
if [ ! -e \$live/vmlinuz ]; then
    search --no-floppy --set=root --file /\$media/vmlinuz
    set live=/\$media
fi

if loadfont \$live/unicode.pf2 ; then
    set gfxmode=auto
    insmod efi_gop
    insmod efi_uga
    insmod gfxterm
    terminal_output gfxterm
fi

menuentry "Debian GNU/Linux Live (Trixie)" --hotkey=d {
    linux \$live/vmlinuz boot=live components live-media-path=\$media ignore_uuid noeject noprompt nonetworking live-config.timezone=Europe/Warsaw loglevel=3
    initrd \$live/initrd.img
}

menuentry "Debian GNU/Linux Live (Debug / Verbose mode)" --hotkey=v {
    linux \$live/vmlinuz boot=live components live-media-path=\$media ignore_uuid noeject noprompt nonetworking live-config.timezone=Europe/Warsaw debug=1
    initrd \$live/initrd.img
}

menuentry "UEFI Firmware Settings" --hotkey=f {
    fwsetup
}
EOF
touch -r "$B/esp/grubx64.efi" "$B/esp/grub.cfg"

echo "################ 3. initrd: unlock-bitlocker without folder names"
# Parts a (microcode) and b (modules) stay as they are: everything before the zstd frame is kept verbatim.
C_AT=$(python3 -I - "$B/esp/initrd.img" <<'PY'
# Walks the uncompressed newc archives (with their zero padding) to the first compressed byte.
import sys
d = open(sys.argv[1], 'rb').read()
pos = 0
while True:
    while pos < len(d) and d[pos] == 0:
        pos += 1
    if d[pos:pos + 6] not in (b'070701', b'070702'):
        break
    while True:
        h = d[pos:pos + 110]
        namesize, filesize = int(h[94:102], 16), int(h[54:62], 16)
        name = d[pos + 110:pos + 110 + namesize - 1]
        pos = (pos + 110 + namesize + 3) & ~3
        pos = (pos + filesize + 3) & ~3
        if name == b'TRAILER!!!':
            break
print(pos if d[pos:pos + 4] == b'\x28\xb5\x2f\xfd' else -1)
PY
)
[ "$C_AT" -gt 0 ] || die "no zstd part in initrd"
head -c "$C_AT" "$B/esp/initrd.img" > "$B/ab.cpio"
tail -c +$((C_AT + 1)) "$B/esp/initrd.img" > "$B/c.zst"
( cd "$B/c" && zstd -dcq "$B/c.zst" | cpio -idm --quiet 2>/dev/null )
[ -x "$B/c/usr/sbin/unlock-bitlocker" ] || die "unlock-bitlocker not in initrd"
meta "$B/c" > "$B/ic.meta.before"; hash "$B/c" > "$B/ic.sha.before"
cat > "$B/c/usr/sbin/unlock-bitlocker" <<'EOF'
#!/bin/sh
# Opens BitLocker volumes read-only so live-boot can reach the image on them.
# The recovery password comes from bitlocker.key on a FAT partition - LinuxLive.exe writes it next to
# the boot loader in the ESP (EFI/<folder of the signed GRUB>) - or from the media folder
# (live-media-path); without one it is asked for on the console. No folder name is assumed.

devices=$(blkid -t TYPE=BitLocker -o device 2>/dev/null)
[ -n "$devices" ] || exit 0

for module in dm_mod dm_crypt dm_zero xts cbc ecb aesni_intel af_alg algif_skcipher algif_hash vfat nls_cp437 nls_ascii; do
    modprobe -q "$module" 2>/dev/null
done

export DM_DISABLE_UDEV=1
mkdir -p /run/cryptsetup
media=$(sed -n 's/.*live-media-path=\([^ ]*\).*/\1/p' /proc/cmdline)
media=${media#/}

# Candidate keys, in RAM only; the directory is removed before the real system starts.
keys=/run/bitlocker-keys
esp=/run/bitlocker-esp
mkdir -p "$keys" "$esp"
chmod 700 "$keys"
n=0
for part in $(blkid -t TYPE=vfat -o device 2>/dev/null); do
    mount -t vfat -o ro "$part" "$esp" 2>/dev/null || continue
    for file in "$esp"/[Ee][Ff][Ii]/*/bitlocker.key ${media:+"$esp/$media/bitlocker.key"}; do
        [ -s "$file" ] || continue
        n=$((n + 1))
        cp "$file" "$keys/$n"
    done
    umount "$esp"
done
rmdir "$esp"

index=0
for device in $devices; do
    name="bitlocker$index"
    index=$((index + 1))
    opened=0
    for key in "$keys"/*; do
        [ -s "$key" ] || continue
        if cryptsetup open --type bitlk --readonly --disable-locks --disable-keyring \
            --key-file "$key" "$device" "$name" 2>/dev/null; then
            opened=1
            break
        fi
    done
    [ "$opened" = 1 ] && continue
    cryptsetup open --type bitlk --readonly --disable-locks --disable-keyring \
        "$device" "$name" </dev/console >/dev/console 2>&1
done

rm -rf "$keys"
exit 0
EOF
chmod 755 "$B/c/usr/sbin/unlock-bitlocker"
sh -n "$B/c/usr/sbin/unlock-bitlocker" || die "unlock-bitlocker syntax"
meta "$B/c" > "$B/ic.meta.after"; hash "$B/c" > "$B/ic.sha.after"
echo "initrd changes:"; diff "$B/ic.sha.before" "$B/ic.sha.after" | grep -E '^[<>]' | awk '{print "  " $1 " " $3}' | sort -u
( cd "$B/c" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o | zstd -19 -T0 -q > "$B/new_c.zst" ) || die "initrd pack"
cat "$B/ab.cpio" "$B/new_c.zst" > "$B/esp/initrd.img"
touch -r "$B/esp/vmlinuz" "$B/esp/initrd.img"
rm -rf "$B/c" "$B/c.zst" "$B/new_c.zst"
# Check: the leading parts are identical, the zstd part is valid and holds the boot scripts.
[ "$(head -c "$C_AT" "$B/esp/initrd.img" | sha256sum)" = "$(sha256sum < "$B/ab.cpio")" ] || die "initrd parts a/b changed"
tail -c +$((C_AT + 1)) "$B/esp/initrd.img" | zstd -tq || die "zstd test"
tail -c +$((C_AT + 1)) "$B/esp/initrd.img" | zstd -dcq | cpio -it --quiet 2>/dev/null |
  grep -cE '^(init|usr/sbin/unlock-bitlocker|usr/lib/live/boot/9990-main\.sh|usr/lib/live/boot/9990-misc-helpers\.sh)$'
rm -f "$B/ab.cpio"

echo "################ 4. image: scripts read the media folder from /proc/cmdline"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || { tail -3 "$B/x.log"; die unsquashfs; }
tail -1 "$B/x.log"; rm -f "$B/x.log"
meta "$R" > "$B/meta.before"; hash "$R" > "$B/sha.before"
GETMEDIA='MEDIA=$(sed -n '\''s/.*live-media-path=\([^ ]*\).*/\1/p'\'' /proc/cmdline); MEDIA=${MEDIA#/}; MEDIA=${MEDIA:-live}'
python3 -I - "$R" "$GETMEDIA" <<'PY' || die "script update"
import sys
root, getmedia = sys.argv[1], sys.argv[2]

def edit(rel, pairs):
    p = root + rel
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        if a not in s:
            sys.exit(f'{rel}: not found: {a!r}')
        s = s.replace(a, b)
    open(p, 'w', encoding='utf-8').write(s)

edit('/usr/local/bin/ssh-import.sh', [
    ('MEDIUM=/run/live/medium\n', 'MEDIUM=/run/live/medium\n' + getmedia + '\n'),
    ('"$MEDIUM/debian-live/authorized_keys"', '"$MEDIUM/$MEDIA/authorized_keys"'),
])
edit('/usr/local/bin/wll-commit', [
    ('# wll-commit: Commits live session modifications to /mnt/win/debian-live/persistence.squashfs',
     '# wll-commit: Commits live session modifications to persistence.squashfs in the media folder\n'
     '# (live-media-path) on the Windows volume, next to filesystem.squashfs.'),
    ('ACTION="${1:-commit}"\n', 'ACTION="${1:-commit}"\n' + getmedia + '\nWIN_DIR="/mnt/win/$MEDIA"\n'),
    ('    WIN_DIR="/mnt/win/debian-live"\n', ''),
    ('WIN_DIR="/mnt/win/debian-live"\n', ''),
    ('=== WinLinLive Persistence Status ===', '=== Linux Live persistence status ==='),
])
edit('/etc/systemd/system/wll-persistence.service', [('WinLinLive Session Persistence Auto-Save', 'Linux Live session persistence auto-save')])
edit('/etc/default/wll-persistence', [('# WinLinLive Persistence Configuration', '# Linux Live persistence configuration')])
PY
for f in usr/local/bin/ssh-import.sh usr/local/bin/wll-commit; do bash -n "$R/$f" || die "$f syntax"; done
grep -rIn -E 'debian-live|EFI/debian|WinLinLive' "$R/usr/local" "$R/etc/systemd/system" "$R/etc/default" && die "folder names left"
meta "$R" > "$B/meta.after"; hash "$R" > "$B/sha.after"
echo "image changes (metadata):"; diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | awk '{print "  " $1 " " $7}' | sort -u
echo "image changes (content):";  diff "$B/sha.before" "$B/sha.after" | grep -E '^[<>]' | awk '{print "  " $3}' | sort -u
START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf "$R" "$B/mk.log"
unsquashfs -s "$B/new.squashfs" 2>/dev/null | grep -E 'Compression|Block size|Number of inodes'

echo "################ 5. ESP package and checks"
( cd "$B/esp" && sha256sum $signed ) > "$B/signed.after"
diff "$B/signed.before" "$B/signed.after" > /dev/null || die "a signed file changed"
echo "signed files byte-identical: $(awk '{print $2}' "$B/signed.after" | tr '\n' ' ')"
( cd "$B/esp" && tar --format=ustar --owner=0 --group=0 --numeric-owner -cf "$B/esp.tar" \
    shimx64.efi grubx64.efi grub.cfg vmlinuz initrd.img unicode.pf2 ) || die "esp.tar"
tar -tvf "$B/esp.tar"

for f in esp.tar new.squashfs; do
  name=$f; [ "$f" = new.squashfs ] && name=filesystem.squashfs
  cp "$B/$f" "$OUT/$name.part" || die "copy $name"
  [ "$(sha256sum < "$B/$f")" = "$(sha256sum < "$OUT/$name.part")" ] || die "copy mismatch $name"
  mv -f "$OUT/$name.part" "$OUT/$name"
  ( cd "$OUT" && sha256sum "$name" > "$name.sha256" )
  rm -f "$B/$f"
done
( cd "$OUT" && cat esp.tar.sha256 filesystem.squashfs.sha256 )
echo "=== BUILD OK"
