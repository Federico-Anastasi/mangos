#!/bin/bash
# Installs mangOS on the running phone: all of mangos/rootfs/, the UI (mangos/web/),
# PIM's instructions (mangos/pim/CLAUDE.md), the Inter fonts and the onnx-asr library; and, if
# the phone lacks them, the packages, Claude Code, the pim user, the Piper voice and the Parakeet
# model. Copies only the files that differ from the phone's, in a single tar, and restarts only
# the services whose files changed; if only the page changed it reloads it (pim reload).
#
# PIM can modify from the phone the files this script installs. If one changed since the last
# installation, copying over it would erase PIM's work: the script stops, and the changes must be
# brought into the repo first. With --overwrite it installs anyway.
#
# Does not install Samsung firmware and kernel modules (build_mangos_rootfs.sh,
# install_modules.sh). To replace data already installed, delete it on the phone. A Parakeet
# encoder built with parakeet_mixed.py ($W/asr/parakeet-v3-mixed) takes the place of the
# downloaded one.
#
# With --into <dir> it writes the files into that directory instead of the phone, with no checks,
# restarts or packages (used by build_mangos_rootfs.sh).
#
# Usage, from WSL: bash scripts/install_mangos.sh [--overwrite | --into <dir>]
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$REPO"
OVERWRITE=
INTO=
case "${1:-}" in
	'') ;;
	--overwrite) OVERWRITE=1 ;;
	--into) INTO=$(realpath -m "${2:?missing directory}") && mkdir -p "$INTO" ;;
	*) sed -n '2,/^[^#]/{/^[^#]/!p}' "$REPO/scripts/install_mangos.sh" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

VERSION=0.2
CHECKSUMS=/var/lib/mangos/installed.md5
VOICE=it_IT-paola-medium
PIPER=https://huggingface.co/rhasspy/piper-voices/resolve/c10ece1aade47bb51c153c893d14e5bf8e5b7117/it/it_IT/paola/medium
PARAKEET=https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx/resolve/8f23f0c03c8761650bdb5b40aaf3e40d2c15f1ce
ENCODER=/opt/mangos/asr/parakeet-v3/encoder-model.int8.onnx
ENCODER_MIXED=$W/asr/parakeet-v3-mixed/encoder-model.int8.onnx
# Data downloaded once: target | file in $W | url | sha256.
DOWNLOADS=(
	"/opt/mangos/voices/$VOICE.onnx|$W/voices/$VOICE.onnx|$PIPER/$VOICE.onnx|6fc918b5a0ea6137382833dddfa567bffbe6a5060c02043c87192ee59c04210c"
	"/opt/mangos/voices/$VOICE.onnx.json|$W/voices/$VOICE.onnx.json|$PIPER/$VOICE.onnx.json|aea19c0a7fce29fbc359b93f10e7902854401e4c95ae2ea328ae516b15d296cf"
	"/opt/mangos/asr/parakeet-v3/config.json|$W/asr/parakeet-v3/config.json|$PARAKEET/config.json|666903c76b9798caf2c210afd4f6cd60b08a8dbf9800ec8d7a3bc0d2148ac466"
	"/opt/mangos/asr/parakeet-v3/vocab.txt|$W/asr/parakeet-v3/vocab.txt|$PARAKEET/vocab.txt|d58544679ea4bc6ac563d1f545eb7d474bd6cfa467f0a6e2c1dc1c7d37e3c35d"
	"/opt/mangos/asr/parakeet-v3/nemo128.onnx|$W/asr/parakeet-v3/nemo128.onnx|$PARAKEET/nemo128.onnx|a9fde1486ebfcc08f328d75ad4610c67835fea58c73ba57e3209a6f6cf019e9f"
	"/opt/mangos/asr/parakeet-v3/decoder_joint-model.int8.onnx|$W/asr/parakeet-v3/decoder_joint-model.int8.onnx|$PARAKEET/decoder_joint-model.int8.onnx|eea7483ee3d1a30375daedc8ed83e3960c91b098812127a0d99d1c8977667a70"
	"$ENCODER|$W/asr/parakeet-v3/encoder-model.int8.onnx|$PARAKEET/encoder-model.int8.onnx|6139d2fa7e1b086097b277c7149725edbab89cc7c7ae64b23c741be4055aff09"
)
DOWNLOAD_TARGETS=$(for d in "${DOWNLOADS[@]}"; do printf "%s " "${d%%|*}"; done)

# Run on the mangOS system (R is its root, empty on the phone).
STATE_SETUP='mkdir -p "$R/etc/mangos" "$R/var/lib/mangos/surfaces" "$R/var/lib/mangos/house" "$R/home/pim/pages" "$R/home/pim/house"
	chmod 755 "$R/etc/mangos"
	[ -s "$R/etc/mangos/entity" ] || printf "name=mangOS\ncolor=blue\n" > "$R/etc/mangos/entity"
	[ -s "$R/etc/mangos/voice" ] || printf "gain=8\n" > "$R/etc/mangos/voice"
	[ ! -e "$R/etc/mangos/claude-token" ] || chmod 600 "$R/etc/mangos/claude-token"'

# The files to install, with their permissions, in a separate directory: mangos/rootfs, the page,
# PIM's instructions, the Inter fonts and the onnx-asr library (which lives in /opt/mangos/asr/lib).
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE" "$STAGE.tar"' EXIT
cp -r mangos/rootfs/. "$STAGE/"
find "$STAGE" -name __pycache__ -prune -exec rm -rf {} +
mkdir -p "$STAGE/usr/share/mangos/web"
install -m 644 mangos/web/* "$STAGE/usr/share/mangos/web/"
install -D -m 644 mangos/pim/CLAUDE.md "$STAGE/home/pim/CLAUDE.md"
sed -i "s/@VERSION@/$VERSION/" "$STAGE/etc/os-release"
download https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip "$W/mangos/dl/inter.zip" \
	9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
download https://files.pythonhosted.org/packages/6a/60/2fa469a2ee674c35ab48821a1039762ae7b9d0b88188ac1012e779477f76/onnx_asr-0.12.0-py3-none-any.whl \
	"$W/mangos/dl/onnx_asr-0.12.0-py3-none-any.whl" 5e7ceca454609819ea7833f61e2302e0c8f6ece4f8a78b66c5daba53cb51de4a
mkdir -p "$STAGE/usr/share/mangos/fonts" "$STAGE/opt/mangos/asr/lib"
python3 - "$W/mangos/dl/inter.zip" "$STAGE/usr/share/mangos/fonts" << 'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for name in ("Inter-Regular.ttf", "Inter-SemiBold.ttf"):
    open(f"{sys.argv[2]}/{name}", "wb").write(z.read(f"extras/ttf/{name}"))
EOF
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
	"$W/mangos/dl/onnx_asr-0.12.0-py3-none-any.whl" "$STAGE/opt/mangos/asr/lib"
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE" -type f -exec chmod 644 {} +
chmod 755 "$STAGE"/usr/bin/* "$STAGE"/etc/init.d/* "$STAGE"/etc/local.d/*

mapfile -t FILE_LIST < <(cd "$STAGE" && find . -type f | sed 's|^\./||' | sort)
mkdir -p "$STAGE/var/lib/mangos"
(cd "$STAGE" && md5sum "${FILE_LIST[@]}") > "$STAGE$CHECKSUMS"
declare -A NEW_SUM
while read -r sum file; do NEW_SUM[$file]=$sum; done < "$STAGE$CHECKSUMS"
SERVICES="$(cd "$STAGE/etc/init.d" && printf '%s ' *)ntpd"

# On the phone: what changed since the last installation, what is already there, what is missing.
OUT=
if [ -z "$INTO" ]; then
	OUT=$(ssh m13 "sh -s -- $(printf '%q ' "${FILE_LIST[@]}")" << EOS
cd / || exit 1
[ ! -f $CHECKSUMS ] || md5sum -c $CHECKSUMS 2> /dev/null | grep ': FAILED\$' | sed 's/^/changed /'
md5sum "\$@" 2> /dev/null | sed 's/^/md5 /'
for f in $DOWNLOAD_TARGETS; do [ -s "\$f" ] || echo "missing \$f"; done
for p in $MANGOS_PACKAGES; do apk info -e "\$p" > /dev/null || { echo packages; break; }; done
[ -e /usr/local/bin/claude ] || echo claude
mkdir -p /opt/mangos/voices /opt/mangos/asr/parakeet-v3
exit 0
EOS
)
	changed=$(grep '^changed ' <<< "$OUT" || true)
	if [ -n "$changed" ] && [ -z "$OVERWRITE" ]; then
		echo "Changed on the phone since the last installation, or left by an installation that was" >&2
		echo "interrupted (bring the changes into the repo, or --overwrite):" >&2
		sed 's/^changed //' <<< "$changed" >&2
		exit 1
	fi
fi

declare -A CURRENT_SUM
while read -r _ sum file; do CURRENT_SUM[$file]=$sum; done < <(grep '^md5 ' <<< "$OUT" || true)
TO_COPY=()
for f in "${FILE_LIST[@]}"; do
	[ "${CURRENT_SUM[$f]:-}" = "${NEW_SUM[$f]}" ] || TO_COPY+=("$f")
done
echo "${#TO_COPY[@]} files to install out of ${#FILE_LIST[@]}"
[ "${#TO_COPY[@]}" -gt 15 ] || echo "${TO_COPY[*]}"

# The service to restart when file $1 changes, if there is one.
service_for() {
	case $1 in
		etc/asound.conf) echo mangos-sound mangos-speak mangos-web ;;
		home/pim/CLAUDE.md|usr/share/mangos/pim/*) echo mangos-agent ;;
		etc/chromium/*) echo mangos-web ;;
		etc/conf.d/ntpd) echo ntpd ;;
		usr/bin/mangos-*|etc/init.d/mangos-*) [ ! -e "$STAGE/etc/init.d/${1##*/}" ] || echo "${1##*/}" ;;
	esac
}
# The watchdog and the networks are not restarted live (restarting the Wi-Fi would cut this very
# connection): the new file takes effect at the next boot.
RESTART=
for f in "${TO_COPY[@]}"; do
	for s in $(service_for "$f"); do
		case "$s" in
			mangos-watchdog|mangos-usbnet|mangos-wifi|mangos-dhcp) ;;
			*) case " $RESTART " in *" $s "*) ;; *) RESTART="$RESTART $s" ;; esac ;;
		esac
	done
done
RELOAD=
case " ${TO_COPY[*]} " in *" usr/share/mangos/web/"*) RELOAD=1 ;; esac
[ -z "$RESTART" ] || echo "to restart:$RESTART"

# Packages, Claude Code and the pim user come before anything is copied: with no network the
# script stops here, with the phone untouched.
if [ -z "$INTO" ]; then
	ssh m13 "sh -s" << EOS
set -e
if [ -n "$(grep -x packages <<< "$OUT" || true)" ]; then apk add --no-cache $MANGOS_PACKAGES < /dev/null; fi
if [ -n "$(grep -x claude <<< "$OUT" || true)" ]; then npm install -g @anthropic-ai/claude-code@$CLAUDE_CODE_VERSION < /dev/null; fi
$CREATE_PIM_USER
EOS
fi

# is_missing <data target>: the data is not yet where it is installed.
is_missing() {
	if [ -n "$INTO" ]; then [ ! -s "$INTO$1" ]; else grep -qx "missing $1" <<< "$OUT"; fi
}
COPIED=
for entry in "${DOWNLOADS[@]}"; do
	IFS='|' read -r target cache url sum <<< "$entry"
	is_missing "$target" || continue
	if [ "$target" = "$ENCODER" ] && [ -s "$ENCODER_MIXED" ]; then
		cache=$ENCODER_MIXED
	else
		download "$url" "$cache" "$sum"
	fi
	echo "installing $target"
	if [ -n "$INTO" ]; then
		install -D -m 644 "$cache" "$INTO$target"
	else
		scp -q -l 80000 "$cache" "m13:$target.part"
		COPIED="$COPIED $target"
	fi
done

tar -C "$STAGE" --owner=0 --group=0 --numeric-owner -cf "$STAGE.tar" "${TO_COPY[@]}" "${CHECKSUMS#/}"
if [ -n "$INTO" ]; then
	tar -xf "$STAGE.tar" -C "$INTO"
	R=$INTO sh -c "$STATE_SETUP"
	exit 0
fi

scp -q -l 80000 "$STAGE.tar" m13:/dev/shm/mangos-install.tar
ssh m13 "sh -s" << EOS
set -e
cd /
tar -xf /dev/shm/mangos-install.tar
rm -f /dev/shm/mangos-install.tar
for d in $COPIED; do mv "\$d.part" "\$d"; done
R=
$STATE_SETUP
chown pim:pim /var/lib/mangos/surfaces /var/lib/mangos/house /home/pim/pages /home/pim/house /home/pim/CLAUDE.md
for s in $SERVICES; do [ -e /etc/runlevels/default/\$s ] || rc-update add \$s default > /dev/null; done
set +e
web=
case " $RESTART " in *" mangos-bridge "*|*" mangos-web "*) web=1 ;; esac
[ -z "\$web" ] || rc-service mangos-web stop > /dev/null
for s in $RESTART; do [ "\$s" = mangos-web ] || rc-service \$s restart > /dev/null; done
for s in $SERVICES; do rc-service \$s status > /dev/null 2>&1 || rc-service \$s start > /dev/null; done
[ -z "$RELOAD" ] || [ -n "\$web" ] || pim reload
sleep 2
failed=0
# Without a saved network the Wi-Fi services do not start, and that is not a failure.
for s in $SERVICES; do
	case \$s in mangos-wifi|mangos-dhcp) [ -s /etc/wpa_supplicant/wpa_supplicant.conf ] || continue ;; esac
	rc-service \$s status || failed=1
done
exit \$failed
EOS
