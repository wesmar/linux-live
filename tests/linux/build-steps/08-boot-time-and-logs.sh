#!/bin/bash
# Boot time and cleanliness fixes found by measuring the running system.
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

echo "################ 1. image"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"
K=$(ls "$R/usr/lib/modules" | head -1)
cp "$R/usr/lib/modules/$K/modules.dep" "$B/full.dep"
cp -a "$R/usr/lib/modules/$K/kernel" "$B/fullmods"

# live-config generates a host key for every HostKey line it finds, only ed25519 is used.
sed -i '/^#HostKey \/etc\/ssh\/ssh_host_\(rsa\|ecdsa\)_key$/d' "$R/etc/ssh/sshd_config"
grep -c 'HostKey' "$R/etc/ssh/sshd_config"

# The zone comes from the kernel command line, written by debian-live.ps1 from Windows.
ln -sfn /usr/share/zoneinfo/Etc/UTC "$R/etc/localtime"

mkdir -p "$R/etc/systemd/journald.conf.d"
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=16M\n' > "$R/etc/systemd/journald.conf.d/10-volatile.conf"
chmod 644 "$R/etc/systemd/journald.conf.d/10-volatile.conf"
rm -rf "$R/var/log/journal"

for u in sysstat.service sysstat-collect.timer sysstat-summary.timer sysstat-rotate.timer apt-listchanges.timer; do
  ln -sf /dev/null "$R/etc/systemd/system/$u"
done
rm -f "$R/etc/systemd/system/multi-user.target.wants/sysstat.service" \
      "$R/etc/systemd/system/timers.target.wants/apt-listchanges.timer"
rm -rf "$R/etc/systemd/system/sysstat.service.wants"

cat > "$R/etc/systemd/system/network-ready.service" <<'EOF'
[Unit]
Description=Hold the console until the network is up
After=wifi-autoconnect.service systemd-networkd.service
Before=getty@tty1.service

[Service]
Type=oneshot
ExecStart=-/usr/lib/systemd/systemd-networkd-wait-online --any --ipv4 --timeout=8
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
chmod 644 "$R/etc/systemd/system/network-ready.service"
ln -sf /etc/systemd/system/network-ready.service "$R/etc/systemd/system/multi-user.target.wants/network-ready.service"
[ -x "$R/usr/lib/systemd/systemd-networkd-wait-online" ] || die "wait-online missing"

python3 -I - "$R/usr/local/bin/wifi-connect.sh" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
for old, new in (
    ("# 3. Wait up to 15 seconds for wireless interface to appear\n",
     "# 3. Wait up to 5 seconds for wireless interface to appear\n"),
    ("for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n", "for i in 1 2 3 4 5; do\n"),
):
    if text.count(old) != 1:
        sys.exit(f"expected one match for: {old!r}")
    text = text.replace(old, new)
open(path, "w").write(text)
PY
sh -n "$R/usr/local/bin/wifi-connect.sh" || die "script syntax"

# systemd skips ldconfig and friends only when the stamps are not older than /usr.
find "$R/var/log" -type f -exec truncate -s 0 {} +
STAMP="$(stat -c %Y "$R/usr")000000000"
for f in etc/.updated var/.updated; do
  cat > "$R/$f" <<EOF
# This file was created by systemd-update-done. Its only
# purpose is to hold a timestamp of the time this directory
# was updated. See man:systemd-update-done.service(8).
TIMESTAMP_NSEC=$STAMP
EOF
  chmod 644 "$R/$f"
  touch -r "$R/usr" "$R/$f"
  tail -1 "$R/$f"
done
stat -c '%Y %n' "$R/usr" | sed "s#$R##"
systemd-analyze --root="$R" verify network-ready.service wifi-autoconnect.service 2>&1 | grep -E 'network-ready|wifi-auto|cycle' | head -5

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
unsquashfs -lln "$B/new.squashfs" usr 2>/dev/null | head -1 | awk '{print "usr in image:", $4, $5}'
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-92

echo "################ 2. initrd"
python3 -I "$HELPER" "$INITRD" "$B" save || die split
mkdir -p "$B/b" "$B/c"
( cd "$B/b" && cpio -idm --quiet < "$B/partb.cpio" 2>/dev/null )
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
MB="$B/b/usr/lib/modules/$K"
MC="$B/c/usr/lib/modules/$K"

python3 -I - "$B/full.dep" "$MB" "$B/fullmods" <<'PY'
import os
import shutil
import sys

full, root, source = sys.argv[1], sys.argv[2], sys.argv[3]
deps = {}
for line in open(full):
    name, _, rest = line.partition(":")
    deps[name.strip()] = rest.split()
for name in deps["kernel/drivers/scsi/hv_storvsc.ko.xz"]:
    target = os.path.join(root, name)
    if not os.path.exists(target):
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(os.path.join(source, name[len("kernel/"):]), target)
        print("restored", name.replace("kernel/", "", 1))
removed = []
changed = True
while changed:
    changed = False
    for name, needed in deps.items():
        path = os.path.join(root, name)
        if os.path.exists(path) and any(not os.path.exists(os.path.join(root, d)) for d in needed):
            os.remove(path)
            removed.append(name)
            changed = True
print(f"modules removed because a dependency is absent: {len(removed)}")
for name in removed[:30]:
    print("  ", name.replace("kernel/", "", 1))
PY
find "$MB/kernel" -type d -empty -delete
created_lib=0
[ -e "$B/b/lib" ] || { ln -s usr/lib "$B/b/lib"; created_lib=1; }
python3 -I - "$MB" <<'PY'
import os
import sys

root = sys.argv[1]
path = os.path.join(root, "modules.order")
kept = [l for l in open(path) if os.path.exists(os.path.join(root, l.strip())) or os.path.exists(os.path.join(root, l.strip() + ".xz"))]
open(path, "w").writelines(kept)
PY
depmod -b "$B/b" "$K" 2>&1 | head -5
missing=0
for m in vfat ntfs3 squashfs overlay loop nvme xhci_pci usb_storage uas sd_mod ahci dm_crypt dm_zero xts aesni_intel af_alg algif_skcipher hv_storvsc hv_vmbus virtio_blk virtio_scsi vmd mmc_block sdhci_pci ufshcd_pci usbhid hid_generic efivarfs nls_cp437 nls_ascii nls_utf8; do
  out=$(modprobe -d "$B/b" -S "$K" --show-depends "$m" 2>&1)
  printf '%s\n' "$out" | grep -qE '^(insmod|builtin) ' || { echo "  MISSING: $m"; missing=$((missing+1)); }
done
echo "missing modules: $missing"
[ "$missing" -eq 0 ] || die "required modules are missing"
python3 -I - "$B/full.dep" "$MB" <<'PY'
import os
import sys

full, root = sys.argv[1], sys.argv[2]
deps = {}
for line in open(full):
    name, _, rest = line.partition(":")
    deps[name.strip()] = rest.split()
bad = [n for n, d in deps.items() if os.path.exists(os.path.join(root, n)) and any(not os.path.exists(os.path.join(root, x)) for x in d)]
print(f"kept modules with an absent dependency: {len(bad)}")
PY
[ "$created_lib" = 1 ] && rm -f "$B/b/lib"
cp -a "$MB"/modules.* "$MC/"

python3 -I - "$B/c/usr/lib/live/boot/9990-main.sh" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
old = "\tNetbase\n"
new = '''\t# systemd reads the zone before live-config runs, a local-time RTC needs it that early.
\tfor _ARG in $(cat /proc/cmdline)
\tdo
\t\tcase "${_ARG}" in
\t\t\tlive-config.timezone=*)
\t\t\t\t_ZONE="${_ARG#*=}"
\t\t\t\tif [ -e "/root/usr/share/zoneinfo/${_ZONE}" ]
\t\t\t\tthen
\t\t\t\t\tln -sf "/usr/share/zoneinfo/${_ZONE}" /root/etc/localtime
\t\t\t\tfi
\t\t\t\t;;
\t\tesac
\tdone

\tNetbase
'''
if text.count(old) != 1:
    sys.exit(f"expected one Netbase call, found {text.count(old)}")
open(path, "w").write(text.replace(old, new))
PY
sh -n "$B/c/usr/lib/live/boot/9990-main.sh" || die "main syntax"

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

cp "$B/new.squashfs" "$IMG" || die "copy image"
cp "$B/new_initrd" "$INITRD" || die "copy initrd"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "image mismatch"
[ "$(sha256sum < "$B/new_initrd")" = "$(sha256sum < "$INITRD")" ] || die "initrd mismatch"
ls -l "$IMG" "$INITRD" | awk '{print $5, $NF}' | sed 's#/mnt/c/debian-live/##'
echo "=== BUILD6 OK"
