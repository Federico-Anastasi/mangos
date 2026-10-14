#!/bin/bash
# Finds the phone and opens the connection to its SSH port: first over the cable, then over Wi-Fi.
#
# It is the ProxyCommand of the "m13" alias in ~/.ssh/config (written by scripts/prepare_env.sh),
# so "ssh m13" works with and without the cable:
#
#   Host m13
#       User root
#       HostKeyAlias m13
#       ProxyCommand bash <repo>/scripts/phone_connect.sh
#
# The Wi-Fi address is assigned by the router and may change: the last known one is in
# $W/phone_wifi_ip. If it stops answering, the same network is searched for the machine that
# presents the phone's key ($W/phone_hostkey). Both are written by scripts/phone_wifi.sh.
# M13_NO_CABLE=1 skips the cable, to try the Wi-Fi path.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CABLE=172.16.42.1
KNOWN_IP=$W/phone_wifi_ip
HOSTKEY=$W/phone_hostkey

is_open() { nc -z -w 1 "$1" 22 2>/dev/null; }
is_the_phone() {
	[ -s "$HOSTKEY" ] && [ "$(ssh-keyscan -T 2 -t ed25519 "$1" 2>/dev/null | cut -d' ' -f2-)" = "$(cat "$HOSTKEY")" ]
}

if [ -z "${M13_NO_CABLE:-}" ] && is_open "$CABLE"; then
	exec nc "$CABLE" 22
fi

IP=$(cat "$KNOWN_IP" 2>/dev/null)
if [ -n "$IP" ] && is_open "$IP" && is_the_phone "$IP"; then
	exec nc "$IP" 22
fi

if [ -n "$IP" ]; then
	NETWORK=${IP%.*}
	for candidate in $(seq 1 254 | xargs -P 64 -I{} sh -c "nc -z -w 1 $NETWORK.{} 22 2>/dev/null && echo $NETWORK.{}"); do
		if is_the_phone "$candidate"; then
			echo "$candidate" > "$KNOWN_IP"
			exec nc "$candidate" 22
		fi
	done
fi

echo "Phone not found over the cable or over Wi-Fi." >&2
exit 1
