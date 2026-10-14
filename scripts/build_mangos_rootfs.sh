#!/bin/bash
# Builds the mangOS system from scratch, as a tar archive: Alpine arm64 base, packages, Claude
# Code, the mangOS files (those of install_mangos.sh), Samsung firmware and kernel modules.
# Extracted into the /mangos directory of the system partition it gives a phone like the
# author's, except what is personal: the Wi-Fi network (scripts/phone_wifi.sh), the subscription
# token (scripts/phone_token.sh), the entity's name and color, pages and furnishings.
#
# scripts/prepare_env.sh and an already built kernel in $W/linux-wifi (build_m13_boot.sh) come
# first: our modules are compiled for that version. The script reports what is missing.
#
# Must run as root, for the chroot with arm64 emulation; downloads and builds are done by the
# normal WSL user (the owner of $W, or user 1000).
#   wsl -u root bash scripts/build_mangos_rootfs.sh <PC's SSH public key> <output archive>
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }
umask 022
if [ -z "${W:-}" ]; then
	BUILD_USER=$(getent passwd 1000 | cut -d: -f1)
	W=$(getent passwd "$BUILD_USER" | cut -d: -f6)/m13
else
	BUILD_USER=$(stat -c %U "$W")
fi
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
PUBKEY=${1:?missing SSH public key}
OUT=$(realpath -m "${2:?missing output archive path}")
ROOT=$W/mangos/rootfs
MANGOS_FILES=$W/mangos/mangos-files
OUR_MODULES=$W/mangos/system-modules

BASE_PACKAGES="alpine-base busybox-extras openssh-server e2fsprogs kmod i2c-tools tzdata ca-certificates nano htop iw \
wpa_supplicant wireless-regdb"
MODULES="mangos/tools/reboot_target mangos/tools/acpm_gpu mangos/tools/acpm_thermal mangos/tools/abox
	mangos/tools/chub mangos/tools/camera $KERNEL_MODULES"

REQUIRED=("$PUBKEY" "$ALPINE_TAR" "$W/linux-wifi/include/config/kernel.release" "$W/firmware/$FIRMWARE_WIFI")
for f in $FIRMWARE_AUDIO $FIRMWARE_SENSORS; do REQUIRED+=("$W/firmware/$f"); done
missing=
for f in "${REQUIRED[@]}"; do
	[ -s "$f" ] || { echo "missing $f" >&2; missing=1; }
done
[ -z "$missing" ] || exit 1
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] ||
	echo ':qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:F' > /proc/sys/fs/binfmt_misc/register

# 1. What is downloaded and compiled, as the normal user.
as_user() {
	runuser -u "$BUILD_USER" -- env W="$W" HOME="$(getent passwd "$BUILD_USER" | cut -d: -f6)" "$@"
}
as_user rm -rf "$MANGOS_FILES" "$OUR_MODULES"
as_user bash "$REPO/scripts/install_mangos.sh" --into "$MANGOS_FILES"
as_user bash "$REPO/scripts/install_modules.sh" --into "$OUR_MODULES" $MODULES > /dev/null

# 2. Alpine base, packages, users, Claude Code.
rm -rf "$ROOT"
mkdir -p "$ROOT"
tar -xzf "$ALPINE_TAR" -C "$ROOT"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
grep -q community "$ROOT/etc/apk/repositories" ||
	sed -n 's|/main$|/community|p' "$ROOT/etc/apk/repositories" >> "$ROOT/etc/apk/repositories"
chroot "$ROOT" /bin/sh -e << EOF
# Under emulation apk cannot set the setuid bit of bbsuid and exits with an error: that is fixed
# from outside below, here we only check that every package is really there.
apk add --no-cache $BASE_PACKAGES $MANGOS_PACKAGES > /var/log/apk-build.log 2>&1 || true
for p in $BASE_PACKAGES $MANGOS_PACKAGES; do
	apk info -e "\$p" > /dev/null || { echo "missing package: \$p" >&2; tail -5 /var/log/apk-build.log >&2; exit 1; }
done
for s in sysfs procfs devfs dmesg udev udev-trigger; do rc-update add \$s sysinit > /dev/null; done
for s in hostname bootmisc syslog modules sysctl loopback; do rc-update add \$s boot > /dev/null; done
for s in sshd local ntpd; do rc-update add \$s default > /dev/null; done
for s in mount-ro killprocs savecache; do rc-update add \$s shutdown > /dev/null; done
$CREATE_PIM_USER
passwd -d root > /dev/null
ln -sf /usr/share/zoneinfo/Europe/Rome /etc/localtime
# The subscription token is added by the user (scripts/phone_token.sh).
npm install -g @anthropic-ai/claude-code@$CLAUDE_CODE_VERSION > /var/log/npm-build.log 2>&1 ||
	{ tail -5 /var/log/npm-build.log >&2; exit 1; }
[ -e /usr/local/bin/claude ] || { echo "Claude Code not installed" >&2; exit 1; }
EOF
chmod 4111 "$ROOT/bin/bbsuid"

# 3. mangOS files, Samsung firmware (stays outside the repo) and kernel modules.
cp -r "$MANGOS_FILES/." "$ROOT/"
install -d "$ROOT/lib/firmware" "$ROOT/lib/modules"
tar -xf "$W/firmware/$FIRMWARE_WIFI" -C "$ROOT/lib/firmware"
for f in $FIRMWARE_AUDIO $FIRMWARE_SENSORS; do cp "$W/firmware/$f" "$ROOT/lib/firmware/"; done
cp -r "$OUR_MODULES/lib/modules/." "$ROOT/lib/modules/"

# 4. mangOS services and file ownership.
chroot "$ROOT" /bin/sh -e << 'EOF'
for f in /etc/init.d/mangos-*; do rc-update add "${f##*/}" default > /dev/null; done
chown -R pim:pim /home/pim /var/lib/mangos/surfaces /var/lib/mangos/house
EOF

# No login terminals on the screen: the screen belongs to the UI.
sed -i 's/^tty[0-9]*::respawn/#&/' "$ROOT/etc/inittab"

install -d -m 700 "$ROOT/root/.ssh"
install -m 600 "$PUBKEY" "$ROOT/root/.ssh/authorized_keys"

rm -f "$ROOT/etc/resolv.conf"
tar -C "$ROOT" --numeric-owner -czf "$OUT" .
echo "$OUT: $(du -h "$OUT" | cut -f1), $(du -sh "$ROOT" | cut -f1) extracted, kernel $(cat "$W/linux-wifi/include/config/kernel.release")"
