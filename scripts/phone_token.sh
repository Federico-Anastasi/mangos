#!/bin/bash
# Puts the Claude subscription token (the one printed by "claude setup-token") on the phone and
# restarts the agent services, which read it only when they start.
#
# The token is pasted here in the terminal: it does not show on screen and ends up only on the
# phone, in /etc/mangos/claude-token, readable only by root.
#
# Usage, from WSL: bash scripts/phone_token.sh
set -euo pipefail

read -r -s -p "Paste the token and press Enter: " TOKEN
echo
if [ -z "$TOKEN" ]; then
	echo "No token received." >&2
	exit 1
fi

printf '%s\n' "$TOKEN" | ssh m13 'umask 077; mkdir -p /etc/mangos; cat > /etc/mangos/claude-token; echo "Token saved on the phone: $(wc -c < /etc/mangos/claude-token) characters."'
ssh m13 'rc-service mangos-agent restart'
