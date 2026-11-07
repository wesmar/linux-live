#!/bin/bash
# Third measured pass: prebuilt locale, faster network wait, console-only service set.
set -uo pipefail
IMG=/mnt/c/debian-live/filesystem.squashfs.new
B=$(mktemp -d /dev/shm/sqbuild.XXXXXX) || exit 1
R=$B/root
cleanup() {
  cd /
  for m in "$R/dev" "$R/proc" "$B"; do mountpoint -q "$m" && umount "$m"; done
  rmdir "$B" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP
die() { echo "FATAL: $*"; exit 1; }
mount -t tmpfs -o size=9g tmpfs "$B" || die tmpfs
meta() { ( cd "$1" && find . -printf '%y %m %U %G %s %p -> %l\n' | LC_ALL=C sort -k6 ); }

unsquashfs -no-progress -d "$R" "$IMG" > "$B/x.log" 2>&1 || die unsquashfs
meta "$R" > "$B/meta.before"
USR_BEFORE=$(stat -c %Y "$R/usr")

echo "=== locale"
grep -n 'en_US.UTF-8\|LIVE_LOCALES=' "$R/usr/lib/live/config/0050-locales" | head -6 | cut -c1-80
sed -i 's/^# *en_US.UTF-8 UTF-8$/en_US.UTF-8 UTF-8/' "$R/etc/locale.gen"
grep -c '^en_US.UTF-8 UTF-8$' "$R/etc/locale.gen"
mount --bind /dev "$R/dev"; mount -t proc proc "$R/proc"
chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C locale-gen 2>&1 | tail -2
chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C locale -a | tr '\n' ' '; echo
umount "$R/proc" "$R/dev"
ls -la "$R/usr/lib/locale/locale-archive" | awk '{print "archive bytes:", $5}'

echo "=== network wait"
cat > "$R/usr/local/bin/network-ready.sh" <<'EOF'
#!/bin/sh
# network-ready.sh - Hold the console until a default route exists, 8 seconds at most.
i=0
while [ "$i" -lt 40 ]; do
    [ -n "$(ip -4 route show default 2>/dev/null)" ] && exit 0
    sleep 0.2
    i=$((i + 1))
done
exit 0
EOF
chmod 755 "$R/usr/local/bin/network-ready.sh"
sh -n "$R/usr/local/bin/network-ready.sh" || die "script syntax"
sed -i 's#^ExecStart=.*#ExecStart=/usr/local/bin/network-ready.sh#' "$R/etc/systemd/system/network-ready.service"
grep -n ExecStart "$R/etc/systemd/system/network-ready.service"

echo "=== services"
for u in cups.service cups.socket cups.path cups-browsed.service colord.service lm-sensors.service smartmontools.service e2scrub_reap.service; do
  ln -sf /dev/null "$R/etc/systemd/system/$u"
done
rm -f "$R"/etc/systemd/system/multi-user.target.wants/{cups.service,cups.path,cups-browsed.service,lm-sensors.service,smartmontools.service,e2scrub_reap.service} \
      "$R"/etc/systemd/system/sockets.target.wants/cups.socket "$R"/etc/systemd/system/printer.target.wants/cups.service
rm -f "$R/etc/systemd/system/smartd.service"

find "$R/var/log" -type f -exec truncate -s 0 {} +
[ "$(stat -c %Y "$R/usr")" = "$USR_BEFORE" ] && echo "/usr mtime unchanged, stamps stay valid" || die "/usr mtime changed"

START=$(date +%s)
mksquashfs "$R" "$B/new.squashfs" -comp xz -Xbcj x86 -b 1M -noappend -no-progress -quiet > "$B/mk.log" 2>&1 || { tail -5 "$B/mk.log"; die mksquashfs; }
echo "mksquashfs time: $(( $(date +%s)-START ))s"
rm -rf --one-file-system "$R"

unsquashfs -no-progress -d "$B/v" "$B/new.squashfs" > "$B/x.log" 2>&1 || die "verify extract"
meta "$B/v" > "$B/meta.after"
rm -rf --one-file-system "$B/v"
diff "$B/meta.before" "$B/meta.after" | grep -E '^[<>]' | cut -c1-92 | head -36

cp "$B/new.squashfs" "$IMG" || die "copy image"
[ "$(sha256sum < "$B/new.squashfs")" = "$(sha256sum < "$IMG")" ] || die "copy mismatch"
ls -l "$IMG" | awk '{print $5}'
echo "=== BUILD7 OK"
