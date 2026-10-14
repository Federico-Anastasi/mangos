#!/bin/bash
# Installs on the running phone the Samsung firmware for audio and motion sensors, the kernel
# modules that use it (abox, chub) and the sound loopback and camera regulator modules the rest
# of mangOS loads (KERNEL_MODULES in common.sh), all compiled for the running kernel. The
# firmware is Samsung's, extracted from the original phone, and stays in $W/firmware, never in
# the repo. abox and chub start at the next reboot (etc/local.d/30-hardware.start, installed by
# install_mangos.sh); right away, with audio stopped, with `ssh m13 "rmmod abox; modprobe abox"`.
#
# Usage, from WSL: bash scripts/install_hardware.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
FILES=()
for f in $FIRMWARE_AUDIO $FIRMWARE_SENSORS; do
	[ -s "$W/firmware/$f" ] || { echo "missing $W/firmware/$f (extract it from the original phone)" >&2; exit 1; }
	FILES+=("$W/firmware/$f")
done
scp -q -l 80000 "${FILES[@]}" m13:/lib/firmware/
bash "$REPO/scripts/install_modules.sh" mangos/tools/abox mangos/tools/chub $KERNEL_MODULES > /dev/null
