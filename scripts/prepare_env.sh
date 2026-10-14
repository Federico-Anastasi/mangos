#!/bin/bash
# Prepares the PC (Ubuntu in WSL) to build and install mangOS: packages, kernel and uniLoader
# sources at the versions pinned in common.sh, reference A13 image, Alpine base, ramdisk programs,
# heimdall (writes partitions in download mode) and the ssh alias m13. Safe to re-run: it skips
# what is already there.
#
# Samsung firmware (touchscreen, Wi-Fi, audio, sensors) is not downloaded: it must be extracted
# from the original phone into $W/firmware, and the script reports what is missing.
#
# Usage, from WSL as a normal user (sudo only for apt): bash scripts/prepare_env.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ "$(id -u)" != 0 ] || { echo "do not run as root: it asks for sudo where needed" >&2; exit 1; }

PACKAGES="build-essential gcc-aarch64-linux-gnu bison flex bc libssl-dev libelf-dev kmod cpio git curl
	ca-certificates python3 python3-pil librsvg2-bin fonts-inter qemu-user-static netcat-openbsd
	openssh-client cmake libusb-1.0-0-dev zlib1g-dev"
missing_packages=
for p in $PACKAGES; do
	dpkg-query -W -f='${Status}' "$p" 2> /dev/null | grep 'ok installed' > /dev/null || missing_packages="$missing_packages $p"
done
if [ -n "$missing_packages" ]; then
	sudo apt-get update -qq
	sudo apt-get install -y $missing_packages
fi

mkdir -p "$W"
DL=$W/mangos/dl
ALPINE_URL=https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH

# checkout_sources <dir> <url> <commit>: brings the directory to the given commit.
checkout_sources() {
	[ -e "$1/.git" ] || { git init -q "$1" && git -C "$1" remote add origin "$2"; }
	[ "$(git -C "$1" rev-parse HEAD 2> /dev/null)" = "$3" ] ||
		{ git -C "$1" fetch -q --depth 1 origin "$3" && git -C "$1" checkout -q --detach "$3"; }
}
checkout_sources "$W/linux-wifi" "$KERNEL_URL" "$KERNEL_COMMIT"
checkout_sources "$W/uniLoader" "$UNILOADER_URL" "$UNILOADER_COMMIT"

download "$ALPINE_URL/releases/aarch64/alpine-minirootfs-$ALPINE_VERSION-aarch64.tar.gz" "$ALPINE_TAR" \
	9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773
download https://github.com/exynos850-mainline/releases/releases/download/devel/4GB-boot-uniLoader-20260929-postmarketOS-a135f.img \
	"$A13_IMAGE" 51a9732486b01463c9fe12fff70dab452014c211460491ecaee41596ea9823ff

# The ramdisk uses a static busybox and e2fsck with its libraries: Alpine packages, latest version.
if [ ! -s "$DL/busybox.static" ] || [ ! -s "$DL/e2fsck/sbin/e2fsck" ]; then
	INDEX=$(curl -fsS "$ALPINE_URL/main/aarch64/")
	TMP=$(mktemp -d)
	trap 'rm -rf "$TMP"' EXIT
	# alpine_package <name> <directory to extract it into>
	alpine_package() {
		local file
		file=$(grep -o "\"$1-[0-9][^\"]*\.apk\"" <<< "$INDEX" | tr -d '"' | head -1)
		[ -n "$file" ] || { echo "package $1 not found in Alpine $ALPINE_BRANCH" >&2; return 1; }
		download "$ALPINE_URL/main/aarch64/$file" "$TMP/$file"
		mkdir -p "$2"
		tar -xzf "$TMP/$file" -C "$2" --warning=no-unknown-keyword
	}
	alpine_package busybox-static "$TMP/busybox"
	for p in musl e2fsprogs e2fsprogs-libs libcom_err libblkid libuuid libeconf; do
		alpine_package "$p" "$TMP/e2fsck"
	done
	install -D -m 755 "$TMP/busybox/bin/busybox.static" "$DL/busybox.static"
	rm -rf "$DL/e2fsck"
	mv "$TMP/e2fsck" "$DL/e2fsck"
fi

HEIMDALL=$W/heimdall-grimler
checkout_sources "$HEIMDALL" https://git.sr.ht/~grimler/Heimdall 8f3044db985fd9710038f04886b51240ddbb2834
if [ ! -x "$HEIMDALL/build/bin/heimdall" ]; then
	{ cmake -S "$HEIMDALL" -B "$HEIMDALL/build" -DDISABLE_FRONTEND=ON -DCMAKE_BUILD_TYPE=Release &&
		cmake --build "$HEIMDALL/build" -j"$(nproc)"; } > "$W/heimdall.log" 2>&1 ||
		{ echo "heimdall does not build:" >&2; tail -n 20 "$W/heimdall.log" >&2; exit 1; }
fi

if ! grep -qs '^Host m13$' ~/.ssh/config; then
	mkdir -p ~/.ssh && chmod 700 ~/.ssh
	cat >> ~/.ssh/config << EOF

Host m13
    User root
    HostKeyAlias m13
    ProxyCommand bash $REPO/scripts/phone_connect.sh
EOF
fi

missing_firmware=
for f in $FIRMWARE_HIMAX $FIRMWARE_WIFI $FIRMWARE_AUDIO $FIRMWARE_SENSORS; do
	[ -s "$W/firmware/$f" ] || { echo "missing $W/firmware/$f (extract it from the original phone)" >&2; missing_firmware=1; }
done
[ -z "$missing_firmware" ] || exit 1
echo "environment ready"
