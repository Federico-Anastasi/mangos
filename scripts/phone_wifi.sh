#!/bin/bash
# Connects the phone to a Wi-Fi network.
#
# Lists the networks the phone sees, pick one by number and type the password here in the
# terminal: it does not show on screen and only the derived key stays on the phone, in
# /etc/wpa_supplicant/wpa_supplicant.conf. Once connected it saves on the PC the address obtained
# and the phone's key, which scripts/phone_connect.sh needs to find it again without the cable.
#
# Usage, from WSL: bash scripts/phone_wifi.sh ["network name"]
# With the name given it only asks for the password.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PHONE=m13

SSID=${1:-}
if [ -z "$SSID" ]; then
	echo "Looking for networks..."
	# One line per network, strongest first: "signal<TAB>name".
	NETWORKS=$(ssh "$PHONE" 'ip link set wlan0 up; iw dev wlan0 scan' | awk '
		/^\tsignal:/ { signal = $2 }
		/^\tSSID: ./ { sub(/^\tSSID: /, ""); if (!($0 in seen) || signal > seen[$0]) seen[$0] = signal }
		END { for (name in seen) printf "%s\t%s\n", seen[name], name }' | sort -t "$(printf '\t')" -k1,1 -g -r | head -12)
	if [ -z "$NETWORKS" ]; then
		echo "The phone sees no networks." >&2
		exit 1
	fi
	echo "$NETWORKS" | awk -F '\t' '{ printf "%2d) %s  (%s dBm)\n", NR, $2, $1 }'
	read -r -p "Network number: " NUMBER
	SSID=$(echo "$NETWORKS" | sed -n "${NUMBER}p" | cut -f2)
	if [ -z "$SSID" ]; then
		echo "Invalid number." >&2
		exit 1
	fi
fi
read -r -s -p "Password for $SSID: " PASS
echo

printf '%s\n%s\n' "$SSID" "$PASS" | ssh "$PHONE" '
	read -r S
	read -r P
	umask 077
	mkdir -p /etc/wpa_supplicant
	CONF=/etc/wpa_supplicant/wpa_supplicant.conf
	# The password must be given as an argument: read from a non-terminal input,
	# wpa_passphrase writes nothing. The saved network is replaced only once the new one is valid.
	wpa_passphrase "$S" "$P" | grep -v "#psk=" > "$CONF.new"
	if ! grep -q "psk=" "$CONF.new"; then
		rm -f "$CONF.new"
		echo "Configuration not written: the password must be 8 to 63 characters."
		exit 1
	fi
	mv "$CONF.new" "$CONF"
	rc-service mangos-wifi restart > /dev/null
	rc-service mangos-dhcp restart > /dev/null
	for _ in $(seq 20); do
		iw dev wlan0 link | grep -q Connected && ip addr show wlan0 | grep -q "inet " && break
		sleep 1
	done
	ip addr show wlan0 | grep "inet " || echo "No address from the network: wrong password or network unreachable."
	iw dev wlan0 link | head -3
'

IP=$(ssh "$PHONE" 'ip addr show wlan0' | sed -n 's/.*inet \([0-9.]*\).*/\1/p')
[ -n "$IP" ] || { echo "The phone has no Wi-Fi address: phone_connect.sh will not be able to find it without the cable." >&2; exit 1; }
echo "$IP" > "$W/phone_wifi_ip"
ssh "$PHONE" 'cut -d" " -f1,2 /etc/ssh/ssh_host_ed25519_key.pub' > "$W/phone_hostkey"
