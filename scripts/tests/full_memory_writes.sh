#!/bin/sh
# Do disk writes come out right when memory is full? Fills memory with ballast in tmpfs, and on
# every round forces a read from disk, writes a file and compares its checksum read back without
# going through memory.
#
# Usage, on the phone: sh full_memory_writes.sh <megabytes> <repetitions> <mode>...
#   file        reads normal files of the system volume (no direct read)
#   partition   reads the userdata partition directly, which the system volume sits on
#   super       reads directly another partition, unrelated to the volume
# The ballast is 1800 MB: on a phone with little free memory it must be reduced.
# The result is in /var/tmp/full-memory-writes.txt.
exec > /var/tmp/full-memory-writes.txt 2>&1
MB=$1; N=$2; shift 2
mkdir -p /var/tmp/ram
mountpoint -q /var/tmp/ram || mount -t tmpfs -o size=1900m tmpfs /var/tmp/ram
trap 'rm -f /dev/shm/orig.bin /var/tmp/result.bin; umount /var/tmp/ram' EXIT
dd if=/dev/urandom of=/var/tmp/ram/ballast bs=1M count=1800 2> /dev/null
dd if=/dev/urandom of=/dev/shm/orig.bin bs=1M count=$MB 2> /dev/null
EXPECTED=$(sha1sum /dev/shm/orig.bin | cut -c1-12)
for mode in "$@"; do
	echo 3 > /proc/sys/vm/drop_caches
	g=1; bad=0
	while [ $g -le $N ]; do
		case $mode in
		file) cat /usr/lib/chromium/chromium > /dev/null 2>&1 ;;
		partition) dd if=/dev/disk/by-partlabel/userdata of=/dev/null bs=1M skip=$((1500 + g * 700)) count=3000 2> /dev/null ;;
		super) dd if=/dev/disk/by-partlabel/super of=/dev/null bs=1M skip=$((g * 300)) count=3000 2> /dev/null ;;
		*) echo "unknown mode: $mode (file, partition or super)"; exit 2 ;;
		esac
		cp /dev/shm/orig.bin /var/tmp/result.bin
		sync
		H=$(dd if=/var/tmp/result.bin bs=1M iflag=direct 2> /dev/null | sha1sum | cut -c1-12)
		[ "$H" = "$EXPECTED" ] || { bad=$((bad + 1)); echo "$mode write $g WRONG"; }
		g=$((g + 1))
	done
	echo "$mode: $bad wrong writes out of $N"
done
echo done
