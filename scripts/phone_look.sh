#!/bin/bash
# Takes a picture, from the PC, of what the phone actually shows on its screen (pim screenshot,
# scaled to 40%) and prints process status, memory and battery temperature.
#
# Usage, from WSL: bash scripts/phone_look.sh <output .png file>
OUTPUT=${1:?missing output .png file}
ssh m13 'pim screenshot /tmp/screen.png 2>&1 | head -2
	echo "cage: $(pidof cage) chromium: $(pidof chromium | wc -w) processes"
	free -m | sed -n 2p
	cat /sys/class/power_supply/sm5714-battery/temp'
scp -q m13:/tmp/screen.png "$OUTPUT"
