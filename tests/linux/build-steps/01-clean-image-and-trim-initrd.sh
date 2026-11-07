#!/bin/bash
# Builds filesystem.squashfs.new and initrd.img.new. Works only in a private tmpfs, removed by the trap.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs
OUTDIR=/mnt/c/debian-live
INITRD="$1"
HELPER="$2"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1

cleanup() {
  cd /
  for m in "$B/root/dev" "$B/root/proc"; do mountpoint -q "$m" && umount "$m"; done
  mountpoint -q "$B" && umount "$B"
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=10g tmpfs "$B" || die "tmpfs"
R=$B/root
PKGS="python3-openssl python3-cryptography python3-bcrypt python3-typing-extensions python3-cffi-backend"
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }
hash() { ( cd "$1" && find . -type f -print0 | xargs -0 -P8 -n200 sha256sum | LC_ALL=C sort -k2 ); }

echo "################ 1. extract"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die "unsquashfs"
tail -1 "$B/x.log"
meta "$R" > "$B/meta.before"
hash "$R" > "$B/sha.before"

echo "################ 2. purge leftover python packages"
rm -f "$R/dev/null"
mount --bind /dev "$R/dev"
mount -t proc proc "$R/proc"
chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root DEBIAN_FRONTEND=noninteractive \
  dpkg --purge $PKGS 2>&1 | tail -8
chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C dpkg --audit
chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C dpkg-query -W -f='${db:Status-Abbrev} ${Package}\n' | grep -v '^ii' | head
umount "$R/proc" "$R/dev"

echo "################ 3. remove test traces"
find "$R" -xdev -type d -name __pycache__ -prune -exec rm -rf {} +
rmdir "$R/mnt/c"
rm -f "$R/run/systemd/systemd-units-load" "$R/etc/apt/sources.list" \
      "$R/var/cache/apt/pkgcache.bin" "$R/var/cache/apt/srcpkgcache.bin" \
      "$R"/var/lib/dpkg/*-old "$R"/var/cache/debconf/*-old "$R"/boot/initrd.img-* "$R/initrd.img" "$R/initrd.img.old"

echo "################ 4. network"
cat > "$R/etc/systemd/network/20-wired.network" <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes

[DHCPv4]
RouteMetric=100

[IPv6AcceptRA]
RouteMetric=100
EOF
cat > "$R/etc/systemd/network/25-wireless.network" <<'EOF'
[Match]
Name=wl*

[Network]
DHCP=yes
IgnoreCarrierLoss=3s

[DHCPv4]
RouteMetric=600

[IPv6AcceptRA]
RouteMetric=600
EOF
cat > "$R/usr/local/bin/update-dns.sh" <<'EOF'
#!/bin/sh
# update-dns.sh - Rebuild /etc/resolv.conf from DHCP leases, keeping public fallbacks.
TMP_DNS=$(mktemp)

for f in /run/systemd/netif/leases/*; do
    if [ -f "$f" ]; then
        grep '^DNS=' "$f" 2>/dev/null | cut -d= -f2 | tr ' ' '\n' >> "$TMP_DNS"
    fi
done
echo "1.1.1.1" >> "$TMP_DNS"
echo "8.8.8.8" >> "$TMP_DNS"
echo "1.0.0.1" >> "$TMP_DNS"

awk 'NF && !seen[$1]++ { print "nameserver " $1 }' "$TMP_DNS" > /etc/resolv.conf
chmod 644 /etc/resolv.conf
rm -f "$TMP_DNS"
EOF
cat > "$R/usr/local/bin/wifi-connect.sh" <<'EOF'
#!/bin/sh
# wifi-connect.sh - Wi-Fi auto-connector for Debian Live
# Routing and DNS are handled by systemd-networkd and update-dns.path.

# 1. Refresh credentials when an offline Windows partition is readable.
#    Otherwise the profiles shipped in the image stay in use.
if [ -d /run/live/medium/Windows ] || [ -d /run/live/medium/windows ]; then
    /usr/bin/regedlin wifi --sync /run/live/medium 2>/dev/null || true
fi

# 2. Quiet kernel console messages
dmesg -n 3 2>/dev/null || true

# 3. Wait up to 15 seconds for wireless interface to appear
IFACE=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    IFACE=$(ls -1 /sys/class/net 2>/dev/null | grep "^wl" | head -n1)
    if [ -n "$IFACE" ]; then
        break
    fi
    sleep 1
done

if [ -z "$IFACE" ]; then
    echo "No wireless interface detected." >&2
    exit 0
fi

echo "Connecting on wireless interface: $IFACE"
ip link set "$IFACE" up 2>/dev/null || true

# 4. Terminate any previous wpa_supplicant
killall -q wpa_supplicant 2>/dev/null && sleep 1

# 5. Start wpa_supplicant in background
/usr/sbin/wpa_supplicant -B -i "$IFACE" -c /etc/wpa_supplicant/wpa_supplicant.conf

exit 0
EOF
cat > "$R/etc/systemd/system/update-dns.path" <<'EOF'
[Unit]
Description=Watch DHCP leases for DNS changes

[Path]
PathChanged=/run/systemd/netif/leases
Unit=update-dns.service

[Install]
WantedBy=multi-user.target
EOF
cat > "$R/etc/systemd/system/update-dns.service" <<'EOF'
[Unit]
Description=Refresh resolv.conf from DHCP leases

[Service]
Type=oneshot
ExecStart=/usr/local/bin/update-dns.sh
EOF
chmod 644 "$R"/etc/systemd/network/2*.network "$R"/etc/systemd/system/update-dns.*
chmod 755 "$R/usr/local/bin/update-dns.sh" "$R/usr/local/bin/wifi-connect.sh"
ln -sf /etc/systemd/system/update-dns.path "$R/etc/systemd/system/multi-user.target.wants/update-dns.path"

# systemd-networkd owns every interface; ifupdown must not start a second DHCP client.
rm -f "$R/etc/systemd/system/multi-user.target.wants/networking.service" \
      "$R/etc/systemd/system/network-online.target.wants/networking.service"
for u in networking.service ifup@.service ifupdown-pre.service ifupdown-wait-online.service; do
  ln -sf /dev/null "$R/etc/systemd/system/$u"
done

echo "################ 5. clock: Windows keeps the RTC in local time"
printf '0.0 0 0.0\n0\nLOCAL\n' > "$R/etc/adjtime"
ln -sfn /usr/share/zoneinfo/Europe/Warsaw "$R/etc/localtime"
[ -f "$R/etc/timezone" ] && echo 'Europe/Warsaw' > "$R/etc/timezone"

echo "################ 6. empty logs again (dpkg just wrote to them)"
find "$R/var/log" -type f -exec truncate -s 0 {} +

for s in update-dns.sh wifi-connect.sh; do sh -n "$R/usr/local/bin/$s" || die "syntax $s"; done
systemd-analyze --root="$R" verify update-dns.path update-dns.service wifi-autoconnect.service 2>&1 | grep -E 'update-dns|wifi-' | head -6

echo "################ 7. pack"
START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die "mksquashfs"; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
ls -l "$IMG" "$B/new.squashfs" | awk '{print $5}'
unsquashfs -s "$B/new.squashfs" | grep -E 'Compression|Block size|Number of (inodes|xattr)|Filesystem size'
rm -rf --one-file-system "$R"

echo "################ 8. verify against the original, file by file"
unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
hash "$B/v" > "$B/sha.after"
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | grep -v __pycache__ > "$B/d.meta" || true
diff "$B/sha.before" "$B/sha.after" | grep -E '^[<>]' | awk '{print $3}' | sort -u | grep -v __pycache__ > "$B/d.sha" || true
echo "entries changed/added/removed (excluding __pycache__): $(cut -c1 "$B/d.meta" | wc -l)"
echo "--- outside the purged packages and documentation"
awk '{print $1, $2, $3, $4":"$5, $6, $7, $8, $9}' "$B/d.meta" | grep -vE 'python3-(bcrypt|cffi|cryptography|openssl|typing)|dist-packages/(bcrypt|cryptography|OpenSSL|typing_extensions|_cffi)|var/lib/dpkg/info/' | cut -c1-110 | head -70
echo "--- regular files with changed content: $(wc -l < "$B/d.sha")"
grep -vE 'python3-(bcrypt|cffi|cryptography|openssl|typing)|dist-packages/(bcrypt|cryptography|OpenSSL|typing_extensions|_cffi)|var/lib/dpkg/info/' "$B/d.sha" | cut -c1-100 | head -40
echo "--- any kernel module or firmware touched?"
grep -cE 'usr/lib/(modules|firmware)/|boot/vmlinuz|usr/bin/regedlin|usr/bin/mc$|usr/sbin/wpa_supplicant' "$B/d.sha" || true

cp "$B/new.squashfs" "$OUTDIR/filesystem.squashfs.new" || die "copy squashfs"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$OUTDIR/filesystem.squashfs.new")" ] || die "copy mismatch"
echo "written: filesystem.squashfs.new"
rm -f "$B/new.squashfs"

echo "################ 9. initrd"
python3 -I "$HELPER" "$INITRD" "$B" save || die "split"
mkdir -p "$B/b" "$B/c"
( cd "$B/b" && cpio -idm --quiet < "$B/partb.cpio" 2>/dev/null )
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
K=$(ls "$B/b/usr/lib/modules" | head -1)
MB="$B/b/usr/lib/modules/$K"
MC="$B/c/usr/lib/modules/$K"

# GPU and network drivers load after the switch to the image, together with their firmware.
rm -rf "$MB/kernel/drivers/gpu" "$MB/kernel/drivers/net" "$MB/kernel/drivers/infiniband"
rm -rf "$B/c/usr/lib/firmware"
cp -a "$MC"/modules.builtin* "$MC/modules.order" "$MB/"
grep -vE '^kernel/drivers/(gpu|net|infiniband)/' "$MB/modules.order" > "$B/order" && cp "$B/order" "$MB/modules.order"

created_lib=0
[ -e "$B/b/lib" ] || { ln -s usr/lib "$B/b/lib"; created_lib=1; }
depmod -b "$B/b" "$K" 2>&1 | head -5
[ "$created_lib" = 1 ] && rm -f "$B/b/lib"
[ -s "$MB/modules.dep" ] || die "depmod"
grep -cE 'drivers/(gpu|net)/' "$MB/modules.dep"
for m in fs/fat/vfat fs/ntfs3/ntfs3 fs/squashfs/squashfs fs/overlayfs/overlay drivers/block/loop drivers/nvme/host/nvme drivers/usb/host/xhci-pci drivers/ata/ahci; do
  printf '%-30s %s\n' "$m" "$(grep -c "^kernel/$m\.ko" "$MB/modules.dep")"
done
cp -a "$MB"/modules.* "$MC/"

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
python3 -I "$HELPER" "$B/new_initrd" "$B/chk" save || die "new initrd structure"
zstd -tq "$B/chk/partc.zst" || die "zstd test"
cpio -it --quiet < "$B/chk/partb.cpio" 2>/dev/null | grep -cE 'vfat|ntfs3|squashfs|overlay|/nvme\.ko'
zstd -dcq "$B/chk/partc.zst" | cpio -it --quiet 2>/dev/null | grep -cE '^(init|usr/lib/live/boot/9990-main\.sh|usr/lib/live/boot/9990-misc-helpers\.sh|scripts/init-top/00_mount_efivarfs)$'
ls -l "$INITRD" "$B/new_initrd" | awk '{print $5}'
cp "$B/new_initrd" "$OUTDIR/initrd.img.new" || die "copy initrd"
echo "written: initrd.img.new"
sha256sum "$OUTDIR/filesystem.squashfs.new" "$OUTDIR/initrd.img.new" | sed 's#/mnt/c/debian-live/##'
echo "=== BUILD OK"
