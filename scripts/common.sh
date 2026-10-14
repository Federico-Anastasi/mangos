# Values and functions shared by the scripts in scripts/. Load with:
#   . "$(dirname "${BASH_SOURCE[0]}")/common.sh"
# Does not set shell options.

# Working directory on the PC, outside the repo: kernel sources, firmware, downloads, images.
W=${W:-$HOME/m13}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Pinned build inputs: a different version is a different system.
KERNEL_URL=https://github.com/exynos850-mainline/linux
KERNEL_COMMIT=497fc766fcc4c9c8051d257e09f7378e98a3f30e      # branch DNM/scsc-wlan: kernel 7.3 with the Samsung Wi-Fi
UNILOADER_URL=https://github.com/exynos850-mainline/uniLoader
UNILOADER_COMMIT=56c45a25619fd2ccdb0826164e719487bb530557
ALPINE_VERSION=3.24.2
ALPINE_BRANCH=v${ALPINE_VERSION%.*}
CLAUDE_CODE_VERSION=2.1.296

# Boot image published for the A13: its header and addresses are reused.
A13_IMAGE=$W/a13-release/4GB-boot-uniLoader-20260929-postmarketOS-a135f.img
ALPINE_TAR=$W/mangos/dl/alpine-minirootfs-$ALPINE_VERSION-aarch64.tar.gz

# Samsung firmware, extracted from the original phone into $W/firmware (never into the repo).
FIRMWARE_HIMAX=himax/hx83112f_boe_m13_f13.bin
FIRMWARE_WIFI=wifi/vendor-wifi.tar
FIRMWARE_AUDIO="audio/calliope_sram.bin audio/calliope_dram.bin audio/calliope_sram_2.bin audio/calliope_dram_2.bin"
FIRMWARE_SENSORS="sensors/shub_nacho_bl.bin sensors/shub_nacho_m13.bin"

# Alpine packages mangOS needs on top of a base Alpine system.
MANGOS_PACKAGES="python3 bash doas curl git ripgrep nodejs npm chromium cage seatd mesa-dri-gallium mesa-egl \
mesa-gbm mesa-gles eudev udev-init-scripts font-inter grim espeak-ng py3-onnxruntime py3-numpy py3-pillow \
py3-websockets sox alsa-utils alsa-lib"

# Modules the kernel builds on request (SYMBOL=path, for install_modules.sh) that mangOS needs: the
# sound loopback of mangos-sound and the camera's regulator.
KERNEL_MODULES="REGULATOR_S2MPB03=drivers/regulator/s2mpb03-regulator.ko
	SOUND=sound/soundcore.ko SND=sound/core/snd.ko SND_TIMER=sound/core/snd-timer.ko
	SND_PCM=sound/core/snd-pcm.ko SND_ALOOP=sound/drivers/snd-aloop.ko"

# Creates the entity's user, an administrator without a password. Runs on the mangOS system.
CREATE_PIM_USER='id pim > /dev/null 2>&1 || adduser -D -g "mangOS entity" -s /bin/bash pim
	mkdir -p /etc/doas.d
	printf "permit nopass pim as root\n" > /etc/doas.d/pim.conf
	chmod 600 /etc/doas.d/pim.conf'

# download <url> <file> [sha256]: writes to <file>.part and renames it only once the download is
# complete and verified; if <file> already exists it only verifies it.
download() {
	local url=$1 file=$2 checksum=${3:-} to_check=$2
	if [ ! -s "$file" ]; then
		mkdir -p "$(dirname "$file")"
		echo "downloading $(basename "$file")"
		to_check=$file.part
		curl -fsSL --retry 3 -o "$to_check" "$url" || { rm -f "$to_check"; return 1; }
	fi
	if [ -n "$checksum" ] && [ "$(sha256sum "$to_check" | cut -d' ' -f1)" != "$checksum" ]; then
		echo "$to_check: SHA-256 checksum does not match the expected one" >&2
		[ "$to_check" = "$file" ] || rm -f "$to_check"
		return 1
	fi
	[ "$to_check" = "$file" ] || mv "$to_check" "$file"
}
