#!/bin/bash
# Root login with the same keys. Reads C:\debian-live, writes the new image to OUT only.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs
OUT="$1"
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root; L=$B/lower; RW=$B/rw; M=$B/merged
cleanup() {
  cd /
  pkill -f 'sshd -D -p 2222' 2>/dev/null
  for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L" "$B"; do mountpoint -q "$m" && umount "$m"; done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=10g tmpfs "$B" || die tmpfs
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"
sed -i 's/^PermitRootLogin no$/PermitRootLogin prohibit-password/' "$R/etc/ssh/sshd_config.d/10-key-only.conf"
grep -c '^PermitRootLogin prohibit-password$' "$R/etc/ssh/sshd_config.d/10-key-only.conf"
python3 -I - "$R/usr/local/bin/ssh-import.sh" <<'PY' || exit 1
import sys

path = sys.argv[1]
text = open(path).read()
pairs = (
    ('# Keys from Users\\*\\.ssh are ignored: the live user has passwordless sudo.\n',
     '# Keys from Users\\*\\.ssh are ignored: these keys open root and the live user.\n'),
    ('chmod 600 "$HOMEDIR/.ssh/authorized_keys"\n',
     'chmod 600 "$HOMEDIR/.ssh/authorized_keys"\n\n'
     'install -d -m 700 /root/.ssh\n'
     'printf \'%s\\n\' "$KEYS" > /root/.ssh/authorized_keys\n'
     'chmod 600 /root/.ssh/authorized_keys\n'),
)
for old, new in pairs:
    if text.count(old) != 1:
        sys.exit(f"expected one match for: {old!r}")
    text = text.replace(old, new)
open(path, "w").write(text)
PY
sh -n "$R/usr/local/bin/ssh-import.sh" || die "script syntax"

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-80

echo "=== login test"
mkdir -p "$L" "$RW/upper" "$RW/work" "$M" "$B/medium/ProgramData/ssh"
mount -t squashfs -o loop,ro "$B/new.squashfs" "$L" || die squash
mount -t overlay overlay -o "lowerdir=$L,upperdir=$RW/upper,workdir=$RW/work" "$M" || die overlay
mount --bind /dev "$M/dev"; mount -t proc proc "$M/proc"; mount -t sysfs sysfs "$M/sys"
mkdir -p "$M/run/live/medium"
mount --bind "$B/medium" "$M/run/live/medium"
run_m() { chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root "$@"; }
run_m useradd -m -u 1000 -s /bin/bash user
ssh-keygen -q -t ed25519 -N '' -f "$B/testkey"
ssh-keygen -q -t ed25519 -N '' -f "$B/hostkey"
cp "$B/testkey.pub" "$B/medium/ProgramData/ssh/administrators_authorized_keys"
cp "$B/hostkey" "$B/medium/ProgramData/ssh/ssh_host_ed25519_key"
cp "$B/hostkey.pub" "$B/medium/ProgramData/ssh/ssh_host_ed25519_key.pub"
run_m sh -c 'echo root:testpass | chpasswd'
run_m /usr/local/bin/ssh-import.sh; echo "ssh-import rc=$?"
stat -c '%A %U %n' "$M/root/.ssh" "$M/root/.ssh/authorized_keys" | sed "s#$M##"
mkdir -p "$M/run/sshd"
run_m /usr/sbin/sshd -t && echo "sshd -t: ok"
run_m /usr/sbin/sshd -D -p 2222 -o ListenAddress=127.0.0.1 &
sleep 2
O="-p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR"
echo "root with key : $(ssh $O -i "$B/testkey" -o BatchMode=yes root@127.0.0.1 'id -un' 2>&1 | tail -1)"
echo "user with key : $(ssh $O -i "$B/testkey" -o BatchMode=yes user@127.0.0.1 'id -un' 2>&1 | tail -1)"
ssh-keygen -q -t ed25519 -N '' -f "$B/badkey"
echo "root wrong key: $(ssh $O -i "$B/badkey" -o IdentitiesOnly=yes -o BatchMode=yes root@127.0.0.1 true 2>&1 | tail -1 | cut -c1-60)"
echo "root password : $(ssh $O -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no -o BatchMode=yes root@127.0.0.1 true 2>&1 | tail -1 | cut -c1-60)"
pkill -f 'sshd -D -p 2222'; sleep 1
rm -f "$B"/testkey* "$B"/hostkey* "$B"/badkey*
for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L"; do umount "$m"; done

mkdir -p "$OUT"
cp "$B/new.squashfs" "$OUT/filesystem.squashfs" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$OUT/filesystem.squashfs")" ] || die "copy mismatch"
ls -l "$OUT/filesystem.squashfs" | awk '{print $5}'
echo "=== BUILD9 OK"
