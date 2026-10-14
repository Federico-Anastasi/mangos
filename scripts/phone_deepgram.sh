#!/bin/bash
# Puts the Deepgram key on the phone, where cloud transcription and voice use it (pim speech
# cloud), and restarts the voice service, which reads it only when it starts.
#
# The key is taken from $W/keys/deepgram if it is there, otherwise it is pasted here in the
# terminal, where it does not show on screen. On the phone it ends up only in
# /etc/mangos/deepgram-key, readable only by root.
#
# Usage, from WSL: bash scripts/phone_deepgram.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if [ -s "$W/keys/deepgram" ]; then
	KEY=$(cat "$W/keys/deepgram")
else
	read -r -s -p "Paste the Deepgram key and press Enter: " KEY
	echo
fi
if [ -z "$KEY" ]; then
	echo "No key received." >&2
	exit 1
fi

printf '%s\n' "$KEY" | ssh m13 'umask 077; mkdir -p /etc/mangos; cat > /etc/mangos/deepgram-key; echo "Key saved on the phone: $(wc -c < /etc/mangos/deepgram-key) characters."'
ssh m13 'rc-service mangos-speak restart'
