#!/bin/bash
# Writes a boot image into the boot or recovery partition of the running phone: copies it into
# the phone's RAM, checks its checksum, writes it, reads it back and compares again. For boot, it
# then reboots.
#
# Test kernels go in recovery and are started with `ssh m13 mangos-reboot recovery`. Boot only
# gets an image that has already worked from there: the script checks it (the same image is in
# recovery and the phone runs its kernel) and refuses otherwise, unless --force is given.
#
# A kernel written to boot that does not start can only be removed with the phone's download mode
# (keys and cable) and `$W/heimdall-grimler/build/bin/heimdall flash --BOOT` with a good image.
#
# Usage, from WSL: bash scripts/flash_boot.sh <image> boot|recovery [--force]
set -euo pipefail
IMG=${1:?boot image to write}
PART=${2:?partition: boot or recovery}
FORCE=${3:-}
case "$PART" in
	boot|recovery) ;;
	*) echo "partition not allowed: $PART (boot or recovery)" >&2; exit 1 ;;
esac
[ -z "$FORCE" ] || [ "$FORCE" = --force ] || { echo "unknown argument: $FORCE" >&2; exit 1; }
[ "$(head -c 8 "$IMG")" = 'ANDROID!' ] || { echo "$IMG is not an Android boot image" >&2; exit 1; }
BYTES=$(stat -c %s "$IMG")
EXPECTED=$(sha256sum "$IMG" | cut -d' ' -f1)

CAPACITY=$(ssh m13 "d=\$(readlink -f /dev/disk/by-partlabel/$PART); echo \$(( \$(cat /sys/class/block/\${d##*/}/size) * 512 ))")
[ "$BYTES" -le "$CAPACITY" ] || { echo "image too large: $BYTES bytes, $PART holds $CAPACITY" >&2; exit 1; }

if [ "$PART" = boot ] && [ -z "$FORCE" ]; then
	IN_RECOVERY=$(ssh m13 "head -c $BYTES /dev/disk/by-partlabel/recovery | sha256sum | cut -d' ' -f1")
	RUNNING=$(ssh m13 uname -r)
	# The image name is part of the kernel version (build_m13_boot.sh).
	NAME=$(basename "$IMG" .img | tr -c 'A-Za-z0-9.\n' '-')
	case "$RUNNING" in
		*"-$NAME"|*"-$NAME-"*) IS_RUNNING=1 ;;
		*) IS_RUNNING= ;;
	esac
	if [ "$IN_RECOVERY" != "$EXPECTED" ] || [ -z "$IS_RUNNING" ]; then
		echo "boot only gets an image already tried from recovery: write it to recovery first, boot it" >&2
		echo "with mangos-reboot recovery and verify it. Recovery holds another image or the phone" >&2
		echo "runs another kernel ($RUNNING). To write it anyway: --force." >&2
		exit 1
	fi
fi

scp -q -l 80000 "$IMG" m13:/dev/shm/boot.img
ssh m13 "
	set -e
	[ \"\$(sha256sum /dev/shm/boot.img | cut -d' ' -f1)\" = $EXPECTED ] || { echo 'copy differs from the original'; exit 1; }
	dd if=/dev/shm/boot.img of=/dev/disk/by-partlabel/$PART bs=1M 2> /dev/null
	rm /dev/shm/boot.img
	sync
	echo 3 > /proc/sys/vm/drop_caches
	READ_BACK=\$(head -c $BYTES /dev/disk/by-partlabel/$PART | sha256sum | cut -d' ' -f1)
	[ \"\$READ_BACK\" = $EXPECTED ] && echo 'written and read back identical' || { echo 'READ-BACK DIFFERS: not rebooting'; exit 1; }
	sync
	[ $PART = boot ] && reboot || true
"
