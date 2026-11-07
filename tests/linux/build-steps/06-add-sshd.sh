#!/bin/bash
# Adds key-only sshd to the image. Keys come from Windows at boot, none are stored in the image.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root; L=$B/lower; RW=$B/rw; M=$B/merged

cleanup() {
  cd /
  pkill -f 'sshd -D -p 2222' 2>/dev/null
  for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L" "$R/dev" "$R/proc" "$R/sys" "$B"; do
    mountpoint -q "$m" && umount "$m"
  done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=10g tmpfs "$B" || die tmpfs
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

echo "################ 1. extract"
unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"

echo "################ 2. install openssh-server"
mount --bind /dev "$R/dev"; mount -t proc proc "$R/proc"; mount -t sysfs sysfs "$R/sys"
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"
run_r() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none "$@"; }
run_r apt-get -qq -o Acquire::Languages=none update 2>&1 | tail -2
run_r apt-get -y -qq --no-install-recommends install openssh-server > "$B/apt.log" 2>&1 || { tail -8 "$B/apt.log"; die apt; }
grep -E '^(Setting up) ' "$B/apt.log" | cut -c1-70
run_r dpkg --audit
run_r sshd -V 2>&1 | head -1
umount "$R/sys" "$R/proc" "$R/dev"

echo "################ 3. configuration"
rm -f "$R/usr/sbin/policy-rc.d" "$R"/etc/ssh/ssh_host_*
cat > "$R/etc/ssh/sshd_config.d/10-key-only.conf" <<'EOF'
HostKey /etc/ssh/ssh_host_ed25519_key
PubkeyAuthentication yes
AuthenticationMethods publickey
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
EOF
cat > "$R/usr/local/bin/ssh-import.sh" <<'EOF'
#!/bin/sh
# ssh-import.sh - Reuse the SSH keys of the Windows installation. The image ships none.
# Only administrators_authorized_keys counts: the live user has passwordless sudo.
SSHDIR=/run/live/medium/ProgramData/ssh
ENTRY=$(getent passwd 1000) || exit 0
OWNER=${ENTRY%%:*}
HOMEDIR=$(echo "$ENTRY" | cut -d: -f6)
SOURCE="$SSHDIR/administrators_authorized_keys"
[ -f "$SOURCE" ] || exit 0

# Windows files may carry CRLF, a BOM or UTF-16.
KEYS=$(tr -d '\000\r\357\273\277\376\377' < "$SOURCE" | grep -E '^[^#]*(ssh-|ecdsa-|sk-)' | sort -u)
[ -n "$KEYS" ] || exit 0

install -d -m 700 -o "$OWNER" -g "$OWNER" "$HOMEDIR/.ssh"
printf '%s\n' "$KEYS" > "$HOMEDIR/.ssh/authorized_keys"
chown "$OWNER:$OWNER" "$HOMEDIR/.ssh/authorized_keys"
chmod 600 "$HOMEDIR/.ssh/authorized_keys"

# The same host key as Windows keeps known_hosts quiet.
if [ -f "$SSHDIR/ssh_host_ed25519_key" ] && [ -f "$SSHDIR/ssh_host_ed25519_key.pub" ]; then
    tr -d '\r' < "$SSHDIR/ssh_host_ed25519_key" > /etc/ssh/ssh_host_ed25519_key
    tr -d '\r' < "$SSHDIR/ssh_host_ed25519_key.pub" > /etc/ssh/ssh_host_ed25519_key.pub
    chmod 600 /etc/ssh/ssh_host_ed25519_key
    chmod 644 /etc/ssh/ssh_host_ed25519_key.pub
fi

touch /run/ssh-keys-imported
exit 0
EOF
cat > "$R/etc/systemd/system/ssh-import.service" <<'EOF'
[Unit]
Description=Import SSH keys from Windows
After=live-config.service local-fs.target
Before=ssh.service

[Service]
Type=oneshot
ExecStart=/usr/local/bin/ssh-import.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
mkdir -p "$R/etc/systemd/system/ssh.service.d"
cat > "$R/etc/systemd/system/ssh.service.d/10-windows-keys.conf" <<'EOF'
[Unit]
After=ssh-import.service
Wants=ssh-import.service
ConditionPathExists=/run/ssh-keys-imported
EOF
chmod 644 "$R/etc/ssh/sshd_config.d/10-key-only.conf" "$R/etc/systemd/system/ssh-import.service" "$R/etc/systemd/system/ssh.service.d/10-windows-keys.conf"
chmod 755 "$R/usr/local/bin/ssh-import.sh"
ln -sf /etc/systemd/system/ssh-import.service "$R/etc/systemd/system/multi-user.target.wants/ssh-import.service"
sh -n "$R/usr/local/bin/ssh-import.sh" || die "script syntax"

for f in 20-wired 25-wireless; do
  sed -i 's/^\[DHCPv4\]$/[DHCPv4]\nClientIdentifier=mac/' "$R/etc/systemd/network/$f.network"
  grep -c 'ClientIdentifier=mac' "$R/etc/systemd/network/$f.network"
done

echo "################ 4. clean"
find "$R" -xdev -type d -name __pycache__ -prune -exec rm -rf {} +
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/apt/archives/*.deb "$R"/var/cache/apt/archives/partial/*
rm -f "$R"/var/cache/apt/*.bin "$R"/var/lib/dpkg/*-old "$R"/var/cache/debconf/*-old "$R"/var/log/apt/*.xz "$R/run/systemd/systemd-units-load"
find "$R/var/log" -type f -exec truncate -s 0 {} +
rm -rf "$R/root/.cache"
find "$R/tmp" "$R/var/tmp" -mindepth 1 -delete 2>/dev/null
wc -c < "$R/etc/machine-id"
ls "$R"/etc/ssh/ssh_host_* 2>/dev/null | wc -l
systemd-analyze --root="$R" verify ssh-import.service ssh.service 2>&1 | grep -E 'ssh' | head -5

echo "################ 5. pack"
START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

echo "################ 6. verify"
unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | grep -vE 'var/lib/dpkg/info/|usr/share/(doc|man|locale)|usr/lib/python|__pycache__' | awk '{print $1,$2,$3,$6,$7,$8,$9}' | cut -c1-88 | head -60

echo "################ 7. functional test in a RAM overlay"
mkdir -p "$L" "$RW/upper" "$RW/work" "$M"
mount -t squashfs -o loop,ro "$B/new.squashfs" "$L" || die squash
mount -t overlay overlay -o "lowerdir=$L,upperdir=$RW/upper,workdir=$RW/work" "$M" || die overlay
mount --bind /dev "$M/dev"; mount -t proc proc "$M/proc"; mount -t sysfs sysfs "$M/sys"
mkdir -p "$M/run/live/medium"
mount --bind /mnt/c "$M/run/live/medium" && mount -o remount,bind,ro "$M/run/live/medium"
run_m() { chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C HOME=/root "$@"; }
run_m useradd -m -u 1000 -s /bin/bash user
run_m sh -c 'echo user:live | chpasswd'
run_m /usr/local/bin/ssh-import.sh; echo "ssh-import rc=$?"
echo "imported keys: $(grep -c . "$M/home/user/.ssh/authorized_keys")"
stat -c '%A %U' "$M/home/user/.ssh" "$M/home/user/.ssh/authorized_keys"
stat -c '%A %U %n' "$M/etc/ssh/ssh_host_ed25519_key" | sed "s#$M##"
ssh-keygen -lf "$M/etc/ssh/ssh_host_ed25519_key.pub" | awk '{print "image host key: ", $2}'
ssh-keygen -lf /mnt/c/ProgramData/ssh/ssh_host_ed25519_key.pub | awk '{print "windows host key:", $2}'
ls -la "$M/run/ssh-keys-imported" | awk '{print "marker:", $NF}' | sed "s#$M##"
ssh-keygen -q -t ed25519 -N '' -f "$B/testkey"
cat "$B/testkey.pub" >> "$M/home/user/.ssh/authorized_keys"
mkdir -p "$M/run/sshd"
run_m /usr/sbin/sshd -t && echo "sshd -t: ok"
run_m /usr/sbin/sshd -T 2>/dev/null | grep -E '^(passwordauthentication|kbdinteractiveauthentication|permitrootlogin|authenticationmethods|pubkeyauthentication) '
run_m /usr/sbin/sshd -D -p 2222 -o ListenAddress=127.0.0.1 &
sleep 2
O="-p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR"
echo "key login: $(ssh $O -i "$B/testkey" -o BatchMode=yes user@127.0.0.1 'echo OK-as-$(id -un)' 2>&1 | tail -1)"
echo "wrong key: $(ssh-keygen -q -t ed25519 -N '' -f "$B/badkey"; ssh $O -i "$B/badkey" -o BatchMode=yes -o IdentitiesOnly=yes user@127.0.0.1 true 2>&1 | tail -1 | cut -c1-70)"
echo "password : $(ssh $O -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no -o BatchMode=yes user@127.0.0.1 true 2>&1 | tail -1 | cut -c1-70)"
echo "root     : $(ssh $O -i "$B/testkey" -o BatchMode=yes root@127.0.0.1 true 2>&1 | tail -1 | cut -c1-70)"
pkill -f 'sshd -D -p 2222'; sleep 1
rm -f "$B"/testkey* "$B"/badkey*
for m in "$M/run/live/medium" "$M/dev" "$M/proc" "$M/sys" "$M" "$L"; do umount "$m"; done

cp "$B/new.squashfs" "$IMG" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "copy mismatch"
ls -l "$IMG" | awk '{print $5}'
sha256sum "$IMG" | cut -c1-64
echo "=== BUILD4 OK"
