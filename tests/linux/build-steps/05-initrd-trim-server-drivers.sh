#!/bin/bash
# Second trim of initrd.img.new: server storage, network filesystems, hwdb. RAM only.
set -uo pipefail
INITRD=/mnt/c/debian-live/initrd.img.new
HELPER="$1"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
cleanup() { cd /; mountpoint -q "$B" && umount "$B"; rmdir "$B" 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=2g tmpfs "$B" || die tmpfs

python3 -I "$HELPER" "$INITRD" "$B" save || die split
mkdir -p "$B/a" "$B/b" "$B/c"
( cd "$B/a" && cpio -idm --quiet < "$B/parta.cpio" 2>/dev/null )
( cd "$B/b" && cpio -idm --quiet < "$B/partb.cpio" 2>/dev/null )
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
K=$(ls "$B/b/usr/lib/modules" | head -1)
MB="$B/b/usr/lib/modules/$K"
MC="$B/c/usr/lib/modules/$K"
KD="$MB/kernel"

echo "=== microcode by CPU group (measurement only)"
python3 -I - "$B/a/kernel/x86/microcode/GenuineIntel.bin" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
laptop = {0x4E, 0x5E, 0x8E, 0x9E, 0xA5, 0xA6, 0xA7, 0x7E, 0x8C, 0x8D, 0x97, 0x9A, 0xBE, 0xB7, 0xBA,
          0xBF, 0xAA, 0xAC, 0xBD, 0xC5, 0xC6, 0xB5, 0x5C, 0x7A, 0x9C, 0x96, 0x8A, 0xCC}
offset, keep, drop, count = 0, 0, 0, 0
while offset + 48 <= len(data):
    sig = struct.unpack_from("<I", data, offset + 12)[0]
    total = struct.unpack_from("<I", data, offset + 32)[0] or 2048
    family = (sig >> 8) & 0xF
    model = ((sig >> 4) & 0xF) | (((sig >> 16) & 0xF) << 4)
    if family == 6 and model in laptop:
        keep += total
    else:
        drop += total
    offset += total
    count += 1
print(f"blobs {count}; laptop CPUs since 2015: {keep / 1048576:.1f} MiB; other: {drop / 1048576:.1f} MiB")
PY

echo "=== modules"
before=$(du -sk "$B/b" | cut -f1)
( cd "$KD/drivers/scsi" && find . -mindepth 1 -maxdepth 1 \
    ! -name 'scsi_mod.ko.xz' ! -name 'scsi_common.ko.xz' ! -name 'sd_mod.ko.xz' ! -name 'sr_mod.ko.xz' \
    ! -name 'sg.ko.xz' ! -name 'virtio_scsi.ko.xz' ! -name 'hv_storvsc.ko.xz' ! -name 'vmw_pvscsi.ko.xz' \
    -exec rm -rf {} + )
( cd "$KD/drivers/md" && find . -mindepth 1 -maxdepth 1 \
    ! -name 'dm-mod.ko.xz' ! -name 'dm-crypt.ko.xz' ! -name 'dm-zero.ko.xz' -exec rm -rf {} + )
( cd "$KD/drivers/block" && find . -mindepth 1 -maxdepth 1 \
    ! -name 'loop.ko.xz' ! -name 'virtio_blk.ko.xz' -exec rm -rf {} + )
( cd "$KD/fs" && rm -rf btrfs xfs nfs nfs_common nfsd lockd netfs f2fs reiserfs jfs jffs2 udf hfs hfsplus )
( cd "$KD" && rm -rf net sound drivers/firewire drivers/nvdimm drivers/watchdog drivers/message drivers/target )
sed -i -E '/^kernel\/(net|sound)\//d; /^kernel\/fs\/(btrfs|xfs|nfs|nfs_common|nfsd|lockd|netfs|f2fs|reiserfs|jfs|jffs2|udf|hfs|hfsplus)\//d; /^kernel\/drivers\/(firewire|nvdimm|watchdog|message|target)\//d' "$MB/modules.order"

created_lib=0
[ -e "$B/b/lib" ] || { ln -s usr/lib "$B/b/lib"; created_lib=1; }
depmod -b "$B/b" "$K" 2>&1 | head -5
echo "--- dependency check of what must load"
missing=0
for m in vfat ntfs3 squashfs overlay loop nvme xhci_pci usb_storage uas sd_mod ahci dm_crypt dm_zero xts aesni_intel af_alg algif_skcipher hv_storvsc hv_vmbus virtio_blk virtio_scsi vmd mmc_block sdhci_pci ufshcd_pci usbhid hid_generic i2c_hid_acpi efivarfs nls_cp437 nls_ascii nls_utf8; do
  out=$(modprobe -d "$B/b" -S "$K" --show-depends "$m" 2>&1)
  if ! printf '%s\n' "$out" | grep -qE '^(insmod|builtin) '; then
    echo "  MISSING: $m: $(printf '%s' "$out" | head -1 | cut -c1-70)"; missing=$((missing+1))
  fi
done
echo "missing modules: $missing"
[ "$created_lib" = 1 ] && rm -f "$B/b/lib"
cp -a "$MB"/modules.* "$MC/"
echo "part B: ${before} KiB -> $(du -sk "$B/b" | cut -f1) KiB"

echo "=== userspace"
rm -f "$B/c/usr/lib/udev/hwdb.bin" "$B/c/usr/sbin/pdata_tools" "$B"/c/usr/sbin/thin_* "$B"/c/usr/sbin/cache_* "$B"/c/usr/sbin/era_*
grep -rlE 'hwdb|pdata_tools|thin_check' "$B/c/scripts" "$B/c/usr/lib/live/boot" 2>/dev/null | sed "s#$B/c/##"
echo "(end of references)"

( cd "$B/b" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o > "$B/new_b.cpio" )
SZ=$(stat -c %s "$B/new_b.cpio"); PAD=$(( (512 - SZ % 512) % 512 ))
[ "$PAD" -gt 0 ] && head -c "$PAD" /dev/zero >> "$B/new_b.cpio"
( cd "$B/c" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o | zstd -19 -T0 -q > "$B/new_c.zst" )
python3 -I - "$B" <<'PY'
import sys
base = sys.argv[1]
part_a = open(base + "/parta.cpio", "rb").read()
part_a += b"\0" * ((512 - len(part_a) % 512) % 512)
with open(base + "/new_initrd", "wb") as out:
    out.write(part_a)
    out.write(open(base + "/new_b.cpio", "rb").read())
    out.write(open(base + "/new_c.zst", "rb").read())
PY
mkdir -p "$B/chk"
python3 -I "$HELPER" "$B/new_initrd" "$B/chk" save || die structure
zstd -tq "$B/chk/partc.zst" || die "zstd test"
[ "$missing" -eq 0 ] || die "required modules are missing, initrd.img.new left unchanged"
cp "$B/new_initrd" "$INITRD" || die copy
ls -l "$INITRD" | awk '{print "initrd.img.new:", $5}'
echo "=== TRIM2 OK"
