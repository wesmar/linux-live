#!/bin/bash
# Moves the image and the initrd to the signed backports kernel 7.2. Works in RAM, writes to OUT only.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs
INITRD=/mnt/c/debian-live/initrd.img
HELPER="$1"
OUT="$2"
K1=6.12.107+deb13-amd64
K2=7.2.11+deb13-amd64
FWURL=https://deb.debian.org/debian/pool/non-free-firmware/f/firmware-nonfree/firmware-iwlwifi_20260916-1_all.deb
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root
cleanup() {
  cd /
  for m in "$R/dev" "$R/proc" "$R/sys" "$B"; do mountpoint -q "$m" && umount "$m"; done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=10g tmpfs "$B" || die tmpfs
mkdir -p "$OUT"
run_r() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none INITRD=No "$@"; }

echo "################ 1. image: kernel $K2"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
mount --bind /dev "$R/dev"; mount -t proc proc "$R/proc"; mount -t sysfs sysfs "$R/sys"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"
printf 'Types: deb\nURIs: http://deb.debian.org/debian\nSuites: trixie-backports\nComponents: main non-free-firmware\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n' > "$R/etc/apt/sources.list.d/backports.sources"
run_r apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -1
run_r apt-get -y -qq --no-install-recommends -t trixie-backports install linux-image-amd64 > "$B/apt.log" 2>&1 || { tail -8 "$B/apt.log"; die "kernel install"; }
grep -E '^Setting up linux' "$B/apt.log" | cut -c1-70
run_r apt-get -y -qq purge "linux-image-$K1" > "$B/purge.log" 2>&1 || { tail -5 "$B/purge.log"; die "purge"; }
run_r sh -c "dpkg-query -W -f='\${db:Status-Abbrev} \${Package}\n' 'linux-*' | grep -v '^un'" | cut -c1-60

echo "################ 2. firmware-iwlwifi"
curl -fsSL --max-time 180 "$FWURL" -o "$R/tmp/fw.deb" || die "firmware download"
sha256sum "$R/tmp/fw.deb" | cut -c1-16
xz -dc /dev/null >/dev/null 2>&1
curl -fsSL --max-time 60 https://deb.debian.org/debian/dists/sid/non-free-firmware/binary-all/Packages.xz | xz -dc | awk 'BEGIN{RS=""} /^Package: firmware-iwlwifi\n/' | sed -n 's/^SHA256: //p' > "$B/fw.sha"
[ "$(sha256sum < "$R/tmp/fw.deb" | cut -d' ' -f1)" = "$(cat "$B/fw.sha")" ] && echo "firmware hash matches the Debian index" || die "firmware hash mismatch"
run_r dpkg -i /tmp/fw.deb 2>&1 | tail -2 | cut -c1-70
rm -f "$R/tmp/fw.deb"
run_r dpkg --audit
ls "$R/usr/lib/firmware" | grep -E 'iwlwifi-(bz-b0-fm-c0-c10[67]|cc-a0-77)' | tr '\n' ' '; echo
umount "$R/sys" "$R/proc" "$R/dev"

echo "################ 3. clean image"
cp "$R/usr/lib/modules/$K2/vmlinuz" "$OUT/vmlinuz" || die "vmlinuz"
ls -l "$OUT/vmlinuz" | awk '{print "vmlinuz bytes:", $5}'
rm -f "$R/usr/sbin/policy-rc.d" "$R/etc/apt/sources.list.d/backports.sources" "$R/vmlinuz" "$R/vmlinuz.old" "$R/initrd.img" "$R/initrd.img.old"
rm -f "$R"/boot/initrd.img-* "$R"/boot/vmlinuz-* "$R"/boot/System.map-* "$R"/boot/config-*
find "$R" -xdev -type d -name __pycache__ -prune -exec rm -rf {} +
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/archives/*.deb "$R"/var/cache/apt/archives/partial/* "$R/root/.cache"
rm -f "$R"/var/cache/apt/*.bin "$R"/var/lib/dpkg/*-old "$R"/var/cache/debconf/*-old "$R"/var/log/apt/*.xz "$R/run/systemd/systemd-units-load" \
      "$R/etc/passwd-" "$R/etc/shadow-" "$R/etc/group-" "$R/etc/gshadow-" "$R/etc/.pwd.lock"
find "$R/var/log" -type f -exec truncate -s 0 {} +
find "$R/tmp" "$R/var/tmp" -mindepth 1 -delete 2>/dev/null
ls "$R/usr/lib/modules"
ls "$R/boot" | wc -l
[ -d "$R/usr/lib/modules/$K1" ] && rm -rf "$R/usr/lib/modules/$K1"
STAMP="$(stat -c %Y "$R/usr")000000000"
for f in etc/.updated var/.updated; do
  sed -i "s/^TIMESTAMP_NSEC=.*/TIMESTAMP_NSEC=$STAMP/" "$R/$f"
  touch -r "$R/usr" "$R/$f"
done
tail -1 "$R/etc/.updated"
cp "$R/usr/lib/modules/$K2/modules.dep" "$B/full.dep"
cp "$R/usr/lib/modules/$K2"/modules.builtin* "$R/usr/lib/modules/$K2/modules.order" "$B/" 2>/dev/null
cp -a "$R/usr/lib/modules/$K2/kernel" "$B/fullmods"

echo "################ 4. initrd"
python3 -I "$HELPER" "$INITRD" "$B" save || die split
mkdir -p "$B/b" "$B/c"
( cd "$B/b" && cpio -idm --quiet < "$B/partb.cpio" 2>/dev/null )
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
MB1="$B/b/usr/lib/modules/$K1"; MB2="$B/b/usr/lib/modules/$K2"
MC1="$B/c/usr/lib/modules/$K1"; MC2="$B/c/usr/lib/modules/$K2"
python3 -I - "$B/full.dep" "$MB1" "$MB2" "$B/fullmods" <<'PY' || exit 1
import os, shutil, sys

full, old, new, source = sys.argv[1:5]
deps = {}
for line in open(full):
    name, _, rest = line.partition(":")
    deps[name.strip()] = rest.split()
by_base = {os.path.basename(n): n for n in deps}
keep, missing = set(), []
for dirpath, _, names in os.walk(os.path.join(old, "kernel")):
    for n in names:
        if n in by_base:
            keep.add(by_base[n])
        else:
            missing.append(n)
keep.add(by_base["ntfs.ko.xz"])
wanted = set(keep)
for n in keep:
    wanted.update(deps[n])
for n in sorted(wanted):
    target = os.path.join(new, n)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copy2(os.path.join(source, n[len("kernel/"):]), target)
print(f"carried over {len(keep)}, with dependencies {len(wanted)}, not in 7.2: {len(missing)}")
print("  ", " ".join(sorted(missing))[:300])
added = sorted(wanted - keep)
print("pulled in as dependencies:", " ".join(os.path.basename(a) for a in added)[:400])
PY
rm -rf "$MB1"
cp -a "$B"/modules.builtin* "$B/modules.order" "$MB2/"
python3 -I - "$MB2" <<'PY'
import os, sys
root = sys.argv[1]
path = os.path.join(root, "modules.order")
kept = [l for l in open(path) if os.path.exists(os.path.join(root, l.strip())) or os.path.exists(os.path.join(root, l.strip() + ".xz"))]
open(path, "w").writelines(kept)
PY
created_lib=0
[ -e "$B/b/lib" ] || { ln -s usr/lib "$B/b/lib"; created_lib=1; }
depmod -b "$B/b" "$K2" 2>&1 | head -5
missing=0
for m in vfat ntfs squashfs overlay loop nvme xhci_pci usb_storage uas sd_mod ahci dm_crypt dm_zero xts af_alg algif_skcipher hv_storvsc hv_vmbus virtio_blk virtio_scsi vmd mmc_block sdhci_pci ufshcd_pci usbhid hid_generic efivarfs nls_cp437 nls_ascii nls_utf8; do
  out=$(modprobe -d "$B/b" -S "$K2" --show-depends "$m" 2>&1)
  printf '%s\n' "$out" | grep -qE '^(insmod|builtin) ' || { echo "  MISSING: $m"; missing=$((missing+1)); }
done
echo "missing modules: $missing"
[ "$created_lib" = 1 ] && rm -f "$B/b/lib"
[ "$missing" -eq 0 ] || die "required modules are missing"
python3 -I - "$B/full.dep" "$MB2" <<'PY'
import os, sys
full, root = sys.argv[1], sys.argv[2]
deps = {}
for line in open(full):
    name, _, rest = line.partition(":")
    deps[name.strip()] = rest.split()
bad = [n for n, d in deps.items() if os.path.exists(os.path.join(root, n)) and any(not os.path.exists(os.path.join(root, x)) for x in d)]
print(f"kept modules with an absent dependency: {len(bad)}")
PY
mv "$MC1" "$MC2"
cp -a "$MB2"/modules.* "$MC2/"
grep -rIl "$K1" "$B/c/conf" "$B/c/scripts" "$B/c/etc" 2>/dev/null | sed "s#$B/c/##"
echo "(end of references to the old kernel)"

python3 -I - "$B/c/usr/lib/live/boot" <<'PY' || exit 1
import sys

base = sys.argv[1]


def patch(name, pairs):
    path = f"{base}/{name}"
    text = open(path).read()
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f"{name}: expected one match, found {text.count(old)}: {old!r}")
        text = text.replace(old, new)
    open(path, "w").write(text)
    print("patched", name)


patch("9990-misc-helpers.sh", (
    ('\t\tif [ "${fstype}" = "ntfs" ] || [ "${fstype}" = "ntfs3" ]; then\n'
     '\t\t\tmountopts="ro,noatime,force,nocase,windows_names,iocharset=utf8"\n',
     '\t\tif [ "${fstype}" = "ntfs" ]; then\n'
     '\t\t\tmountopts="ro,noatime,nocase,windows_names,iocharset=utf8"\n'),
    ('\t\tmount -t ntfs3 -o ro,noatime,force,iocharset=utf8 "${devname}" ${mountpoint} 2>/dev/null || \\\n'
     '\t\tmount -t ntfs3 -o ro,noatime,force "${devname}" ${mountpoint} 2>/dev/null || \\\n', ''),
    ('\tif [ "${fstype}" = "ntfs" ] || [ "${fstype}" = "ntfs3" ]; then\n'
     '\t\tmodprobe -q nls_utf8 2>/dev/null || true\n'
     '\t\tmodprobe -q nls_cp437 2>/dev/null || true\n'
     '\t\tmodprobe -q ntfs3 2>/dev/null || true\n',
     '\tif [ "${fstype}" = "ntfs" ]; then\n'
     '\t\tmodprobe -q nls_utf8 2>/dev/null || true\n'
     '\t\tmodprobe -q nls_cp437 2>/dev/null || true\n'
     '\t\tmodprobe -q ntfs 2>/dev/null || true\n'),
    ('\tif [ "$t" = "ntfs" ]; then\n\t\techo "ntfs3"\n\telse\n\t\techo "$t"\n\tfi\n', '\techo "$t"\n'),
))
patch("9990-main.sh", (
    ('\t\t\tmodprobe -q ntfs3 2>/dev/null || true\n', '\t\t\tmodprobe -q ntfs 2>/dev/null || true\n'),
))
PY
sh -n "$B/c/usr/lib/live/boot/9990-misc-helpers.sh" && sh -n "$B/c/usr/lib/live/boot/9990-main.sh" && echo "live-boot syntax ok"
grep -c 'ntfs3' "$B/c/usr/lib/live/boot/9990-misc-helpers.sh" "$B/c/usr/lib/live/boot/9990-main.sh"

( cd "$B/b" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o > "$B/new_b.cpio" )
SZ=$(stat -c %s "$B/new_b.cpio"); PAD=$(( (512 - SZ % 512) % 512 ))
[ "$PAD" -gt 0 ] && head -c "$PAD" /dev/zero >> "$B/new_b.cpio"
( cd "$B/c" && find . -mindepth 1 | LC_ALL=C sort | cpio --quiet -H newc -o | zstd -19 -T0 -q > "$B/new_c.zst" )
python3 -I - "$B" "$OUT/initrd.img" <<'PY'
import sys
base, target = sys.argv[1], sys.argv[2]
part_a = open(base + "/parta.cpio", "rb").read()
part_a += b"\0" * ((512 - len(part_a) % 512) % 512)
with open(target, "wb") as out:
    out.write(part_a)
    out.write(open(base + "/new_b.cpio", "rb").read())
    out.write(open(base + "/new_c.zst", "rb").read())
PY
mkdir -p "$B/chk"
python3 -I "$HELPER" "$OUT/initrd.img" "$B/chk" save || die structure
zstd -tq "$B/chk/partc.zst" || die "zstd test"
cpio -it --quiet < "$B/chk/partb.cpio" 2>/dev/null | grep -cE 'fs/ntfs/ntfs\.ko|md/dm-crypt|fs/fat/vfat|nvme/host/nvme\.ko'
rm -rf "$B/b" "$B/c" "$B/chk" "$B/fullmods" "$B"/*.cpio "$B"/*.zst

echo "################ 5. pack"
START=$(date +%s)
mksquashfs "$R" "$OUT/filesystem.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
ls -l "$OUT" | awk 'NR>1 {print $5, $NF}'
echo "=== BUILD8 OK"
