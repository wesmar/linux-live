#!/bin/bash
# Adds C:\debian-live\authorized_keys as a second SSH key source and tests both situations.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root; L=$B/lower; RW=$B/rw; M=$B/merged

cleanup() {
  cd /
  for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L" "$B"; do
    mountpoint -q "$m" && umount "$m"
  done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=9g tmpfs "$B" || die tmpfs
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"

python3 -I - "$R/usr/local/bin/ssh-import.sh" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
replacements = (
    ("# Only administrators_authorized_keys counts: the live user has passwordless sudo.\n",
     "# Keys from Users\\*\\.ssh are ignored: the live user has passwordless sudo.\n"),
    ('SSHDIR=/run/live/medium/ProgramData/ssh\n',
     'MEDIUM=/run/live/medium\nSSHDIR=$MEDIUM/ProgramData/ssh\n'),
    ('SOURCE="$SSHDIR/administrators_authorized_keys"\n[ -f "$SOURCE" ] || exit 0\n\n', '\n'),
    ("KEYS=$(tr -d '\\000\\r\\357\\273\\277\\376\\377' < \"$SOURCE\" | grep -E '^[^#]*(ssh-|ecdsa-|sk-)' | sort -u)\n",
     "KEYS=$(cat \"$SSHDIR/administrators_authorized_keys\" \"$MEDIUM/debian-live/authorized_keys\" 2>/dev/null |\n"
     "    tr -d '\\000\\r\\357\\273\\277\\376\\377' | grep -E '^[^#]*(ssh-|ecdsa-|sk-)' | sort -u)\n"),
)
for old, new in replacements:
    if text.count(old) != 1:
        sys.exit(f"expected one match for: {old!r}")
    text = text.replace(old, new)
open(path, "w").write(text)
PY
sh -n "$R/usr/local/bin/ssh-import.sh" || die "script syntax"
cat "$R/usr/local/bin/ssh-import.sh"
rm -f "$R/etc/passwd-" "$R/etc/shadow-" "$R/etc/group-" "$R/etc/gshadow-" "$R/etc/subuid-" "$R/etc/subgid-" "$R/etc/.pwd.lock"

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

echo "=== functional tests with a fake Windows partition"
mkdir -p "$L" "$RW/upper" "$RW/work" "$M" "$B/medium"
mount -t squashfs -o loop,ro "$B/new.squashfs" "$L" || die squash
mount -t overlay overlay -o "lowerdir=$L,upperdir=$RW/upper,workdir=$RW/work" "$M" || die overlay
mount --bind /dev "$M/dev"; mount -t proc proc "$M/proc"; mount -t sysfs sysfs "$M/sys"
mkdir -p "$M/run/live/medium"
mount --bind "$B/medium" "$M/run/live/medium"
run_m() { chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root "$@"; }
run_m useradd -m -u 1000 -s /bin/bash user
check() {
  rm -f "$M/run/ssh-keys-imported" "$M/home/user/.ssh/authorized_keys" "$M"/etc/ssh/ssh_host_ed25519_key*
  run_m /usr/local/bin/ssh-import.sh
  printf '%-34s rc=%s marker=%s keys=%s hostkey=%s\n' "$1" "$?" \
    "$([ -e "$M/run/ssh-keys-imported" ] && echo yes || echo no)" \
    "$(grep -c . "$M/home/user/.ssh/authorized_keys" 2>/dev/null || echo 0)" \
    "$([ -e "$M/etc/ssh/ssh_host_ed25519_key" ] && echo yes || echo no)"
}
ssh-keygen -q -t ed25519 -N '' -f "$B/k1"; ssh-keygen -q -t ed25519 -N '' -f "$B/k2"
check "no OpenSSH, no file"
mkdir -p "$B/medium/debian-live"
{ printf '\357\273\277# comment\r\n'; sed 's/$/\r/' "$B/k1.pub"; } > "$B/medium/debian-live/authorized_keys"
check "only authorized_keys (BOM, CRLF)"
mkdir -p "$B/medium/ProgramData/ssh"
cp "$B/k2.pub" "$B/medium/ProgramData/ssh/administrators_authorized_keys"
cp "$B/k2" "$B/medium/ProgramData/ssh/ssh_host_ed25519_key"
cp "$B/k2.pub" "$B/medium/ProgramData/ssh/ssh_host_ed25519_key.pub"
check "both sources, Windows host key"
rm "$B/medium/debian-live/authorized_keys"
python3 -I - "$B/k1.pub" "$B/medium/ProgramData/ssh/administrators_authorized_keys" <<'PY'
import sys
open(sys.argv[2], "wb").write(open(sys.argv[1]).read().encode("utf-16"))
PY
check "Windows file saved as UTF-16"
rm -f "$B"/k1* "$B"/k2*
for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L"; do umount "$m"; done

unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
ls "$B"/v/etc/ssh/ssh_host_* 2>/dev/null | wc -l
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-80

cp "$B/new.squashfs" "$IMG" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "copy mismatch"
ls -l "$IMG" | awk '{print $5}'
echo "=== BUILD5 OK"
