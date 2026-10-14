#!/bin/sh
# Does the disk always return the same data? Reads the same files three times, each time making
# the kernel forget its in-memory copies, and compares the checksums.
#
# Usage, on the phone: sh file_rereads.sh
exec 2>&1
find /usr/lib /usr/bin /sbin /lib -xdev -type f | sort > /var/tmp/list.txt
echo "files: $(wc -l < /var/tmp/list.txt), $(du -shc $(cat /var/tmp/list.txt) 2> /dev/null | tail -1 | cut -f1)"
for round in 1 2 3; do
	sync
	echo 3 > /proc/sys/vm/drop_caches
	xargs sha1sum < /var/tmp/list.txt > /var/tmp/round-$round.txt 2> /var/tmp/round-$round.err
	echo "round $round: $(wc -l < /var/tmp/round-$round.txt) checksums, $(wc -l < /var/tmp/round-$round.err) read errors"
done
echo "different between round 1 and 2: $(diff /var/tmp/round-1.txt /var/tmp/round-2.txt | grep -c '^+[0-9a-f]')"
echo "different between round 2 and 3: $(diff /var/tmp/round-2.txt /var/tmp/round-3.txt | grep -c '^+[0-9a-f]')"
diff /var/tmp/round-1.txt /var/tmp/round-2.txt | grep '^[+-][0-9a-f]' | cut -c1-12,43- | head -6
head -3 /var/tmp/round-1.err
dmesg | grep -i -E "mmc0|crc|timeout|ext4-fs.*(error|warn)" | tail -5 | cut -c1-150
