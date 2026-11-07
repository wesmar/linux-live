#!/bin/bash
# Adds BitLocker unlock support to initrd.img.new. Debian's cryptsetup is fetched into a RAM overlay.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
INITRD=/mnt/c/debian-live/initrd.img.new
HELPER="$1"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
L=$B/lower; RW=$B/rw; M=$B/merged

cleanup() {
  cd /
  for m in "$M/dev" "$M/proc" "$M/sys" "$M" "$L" "$B"; do
    mountpoint -q "$m" && umount "$m"
  done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }

mount -t tmpfs -o size=5g tmpfs "$B" || die tmpfs
mkdir -p "$L" "$RW/upper" "$RW/work" "$M" "$B/b" "$B/c"
mount -t squashfs -o loop,ro "$IMG" "$L" || die squash
mount -t overlay overlay -o "lowerdir=$L,upperdir=$RW/upper,workdir=$RW/work" "$M" || die overlay
mkdir -p "$M/dev"
mount --bind /dev "$M/dev"
mount -t proc proc "$M/proc"
mount -t sysfs sysfs "$M/sys"
run_in() { chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root DEBIAN_FRONTEND=noninteractive "$@"; }

echo "################ 1. fetch cryptsetup into the RAM overlay"
run_in apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -3
run_in apt-get -y -qq --no-install-recommends install cryptsetup-bin > "$B/apt.log" 2>&1 || { tail -8 "$B/apt.log"; die "apt"; }
grep -E '^(Setting up|Unpacking) ' "$B/apt.log" | cut -c1-70 | head -8
run_in cryptsetup --version
run_in sh -c 'cryptsetup --help 2>/dev/null | grep -ci bitlk'

echo "################ 2. unpack initrd"
python3 -I "$HELPER" "$INITRD" "$B" save || die split
( cd "$B/b" && cpio -idm --quiet < "$B/partb.cpio" 2>/dev/null )
( cd "$B/c" && zstd -dcq "$B/partc.zst" | cpio -idm --quiet 2>/dev/null )
K=$(ls "$B/b/usr/lib/modules" | head -1)
MB="$B/b/usr/lib/modules/$K"
MC="$B/c/usr/lib/modules/$K"
LIB="$B/c/usr/lib/x86_64-linux-gnu"
[ -x "$B/c/usr/sbin/blkid" ] && echo "blkid: full util-linux" || echo "blkid: MISSING in usr/sbin"
ls -la "$B/c/usr/sbin/blkid" "$B/c/usr/bin/mount" "$B/c/usr/sbin/modprobe" 2>&1 | awk '{print $NF, $(NF-2), $(NF-1)}' | cut -c1-70

echo "################ 3. binary and libraries"
cp -L "$M/usr/sbin/cryptsetup" "$B/c/usr/sbin/cryptsetup" || die cp
for lib in $(run_in ldd /usr/sbin/cryptsetup | awk '/=> \//{print $3} /^[[:space:]]*\//{print $1}' | sort -u); do
  name=$(basename "$lib"); src="$M$lib"
  if [ -e "$LIB/$name" ] && cmp -s "$src" "$LIB/$name"; then state=same
  elif [ -e "$LIB/$name" ]; then
    case "$name" in libc.so.6|ld-linux-x86-64.so.2) die "$name differs between image and initrd";; esac
    cp -L "$src" "$LIB/$name"; state=replaced
  elif [ "$name" = ld-linux-x86-64.so.2 ]; then state=same
  else cp -L "$src" "$LIB/$name"; state=added; fi
  printf '  %-28s %s\n' "$name" "$state"
done

echo "################ 4. kernel modules"
for m in dm_mod dm_crypt dm_zero xts cbc ecb aesni_intel aes_generic af_alg algif_skcipher algif_hash; do
  modprobe -d "$L" -S "$K" --show-depends "$m" 2>&1
done > "$B/deps.txt"
grep -cE '^builtin' "$B/deps.txt"
added=0
for p in $(awk '$1=="insmod"{print $2}' "$B/deps.txt" | sort -u); do
  rel=${p#*/modules/$K/}
  if [ ! -e "$MB/$rel" ]; then
    mkdir -p "$(dirname "$MB/$rel")"
    cp "$M/usr/lib/modules/$K/$rel" "$MB/$rel" || die "module $rel"
    added=$((added+1)); echo "  $rel" | sed 's#kernel/##'
  fi
done
echo "modules added: $added"

created_lib=0
[ -e "$B/b/lib" ] || { ln -s usr/lib "$B/b/lib"; created_lib=1; }
depmod -b "$B/b" "$K" 2>&1 | head -5
[ "$created_lib" = 1 ] && rm -f "$B/b/lib"
grep -c 'dm-crypt' "$MB/modules.dep"
cp -a "$MB"/modules.* "$MC/"

echo "################ 5. unlock script and live-boot patch"
cat > "$B/c/usr/sbin/unlock-bitlocker" <<'EOF'
#!/bin/sh
# Opens BitLocker volumes read-only so live-boot can reach the image on them.
# The recovery key comes from bitlocker.key on the DEBIANLIVE partition, or from the console.

devices=$(blkid -t TYPE=BitLocker -o device 2>/dev/null)
[ -n "$devices" ] || exit 0

for module in dm_mod dm_crypt dm_zero xts cbc ecb aesni_intel af_alg algif_skcipher algif_hash vfat nls_cp437 nls_ascii; do
    modprobe -q "$module" 2>/dev/null
done

export DM_DISABLE_UDEV=1
mkdir -p /run/cryptsetup
key=/run/bitlocker.key
boot=$(blkid -t LABEL=DEBIANLIVE -o device 2>/dev/null | head -n 1)

if [ -n "$boot" ]; then
    mkdir -p /run/debianlive
    if mount -t vfat -o ro "$boot" /run/debianlive 2>/dev/null; then
        cp /run/debianlive/debian-live/bitlocker.key "$key" 2>/dev/null
        umount /run/debianlive
    fi
    rmdir /run/debianlive
fi

index=0
for device in $devices; do
    name="bitlocker$index"
    index=$((index + 1))
    if [ -s "$key" ] && cryptsetup open --type bitlk --readonly --disable-locks --disable-keyring \
        --key-file "$key" "$device" "$name" 2>/dev/null; then
        continue
    fi
    cryptsetup open --type bitlk --readonly --disable-locks --disable-keyring \
        "$device" "$name" </dev/console >/dev/console 2>&1
done

rm -f "$key"
exit 0
EOF
chmod 755 "$B/c/usr/sbin/unlock-bitlocker"
sh -n "$B/c/usr/sbin/unlock-bitlocker" || die "script syntax"

python3 -I - "$B/c/usr/lib/live/boot" <<'PY' || exit 1
import sys

base = sys.argv[1]


def patch(name, old, new):
    path = f"{base}/{name}"
    text = open(path).read()
    if text.count(old) != 1:
        sys.exit(f"{name}: expected one match, found {text.count(old)}")
    open(path, "w").write(text.replace(old, new))
    print("patched", name)


patch(
    "9990-main.sh",
    "\t\t\tudevadm settle --timeout=10 2>/dev/null || true\n",
    "\t\t\tudevadm settle --timeout=10 2>/dev/null || true\n\t\t\t/usr/sbin/unlock-bitlocker || true\n",
)
patch(
    "9990-misc-helpers.sh",
    "\t# or do the scan of block devices\n",
    "\t# device-mapper nodes are skipped by the block device scan below\n"
    "\tfor dev in /dev/mapper/bitlocker*\n"
    "\tdo\n"
    "\t\t[ -e \"${dev}\" ] || continue\n"
    "\t\tif check_dev \"null\" \"${dev}\" \"skip_uuid_check\"\n"
    "\t\tthen\n"
    "\t\t\treturn 0\n"
    "\t\tfi\n"
    "\tdone\n\n"
    "\t# or do the scan of block devices\n",
)
PY
sh -n "$B/c/usr/lib/live/boot/9990-main.sh" && sh -n "$B/c/usr/lib/live/boot/9990-misc-helpers.sh" && echo "live-boot syntax ok"
sed -n '/^check_dev/,/+6p' "$B/c/usr/lib/live/boot/9990-misc-helpers.sh" | head -8 | cut -c1-70

echo "################ 6. run cryptsetup from the initrd tree"
cp -a "$B/c" "$B/test"
chroot "$B/test" /usr/sbin/cryptsetup --version || die "cryptsetup does not start in the initrd tree"
chroot "$B/test" /usr/sbin/cryptsetup --help 2>/dev/null | grep -ci bitlk
rm -rf "$B/test"

echo "################ 7. repack"
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
python3 -I "$HELPER" "$B/new_initrd" "$B/chk" save || die "structure"
zstd -tq "$B/chk/partc.zst" || die "zstd test"
zstd -dcq "$B/chk/partc.zst" | cpio -it --quiet 2>/dev/null | grep -cE '^usr/sbin/(cryptsetup|unlock-bitlocker)$'
cpio -it --quiet < "$B/chk/partb.cpio" 2>/dev/null | grep -cE 'md/dm-(crypt|zero|mod)'
cp "$B/new_initrd" "$INITRD" || die copy
ls -l "$INITRD" | awk '{print $5}'
sha256sum "$INITRD" | cut -c1-64
echo "=== BITLOCKER INITRD OK"
