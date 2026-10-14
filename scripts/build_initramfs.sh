#!/bin/bash
# Builds the mangOS boot ramdisk: static arm64 busybox, e2fsck, mangos/initramfs/init and the boot
# screen (branding/splash.svg; needs rsvg-convert, Pillow and the Inter font).
# Busybox and e2fsck are downloaded by scripts/prepare_env.sh.
#
# Usage, from WSL: bash scripts/build_initramfs.sh <output ramdisk>
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
OUT=${1:?missing output ramdisk path}
DL=$W/mangos/dl

[ -s "$DL/busybox.static" ] && [ -s "$DL/e2fsck/sbin/e2fsck" ] ||
	{ echo "busybox and e2fsck missing in $DL: run scripts/prepare_env.sh" >&2; exit 1; }
fc-list | grep Inter > /dev/null ||
	{ echo "Inter font missing (package fonts-inter): the boot screen would use another font" >&2; exit 1; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T"/{bin,dev,proc,sys,run,sysroot,newroot,lib,etc}
install -m 755 "$DL/busybox.static" "$T/bin/busybox"
install -m 755 "$DL/e2fsck/sbin/e2fsck" "$T/bin/e2fsck"
cp -L "$DL/e2fsck/lib/ld-musl-aarch64.so.1" "$T/lib/"
for lib in libext2fs.so.2 libe2p.so.2 libcom_err.so.2 libblkid.so.1 libuuid.so.1 libeconf.so.0; do
	cp -L "$(find "$DL/e2fsck" -name "$lib" | head -1)" "$T/lib/"
done
ln -s ld-musl-aarch64.so.1 "$T/lib/libc.musl-aarch64.so.1"
# Without this file e2fsck warns that it cannot find the list of mounted file systems.
: > "$T/etc/mtab"
# The clock may still be wrong this early in boot: without this, e2fsck would take a mount time
# "in the future" for an error and run the full check every time.
printf '[options]\nbroken_system_clock = 1\n' > "$T/etc/e2fsck.conf"
install -m 755 "$REPO/mangos/initramfs/init" "$T/init"
# Boot screen as a raw framebuffer image (1080 x 2408, 4 bytes per pixel, BGRX).
rsvg-convert -w 1080 -h 2408 "$REPO/branding/splash.svg" | python3 -c '
import sys, io, gzip
from PIL import Image
im = Image.open(io.BytesIO(sys.stdin.buffer.read())).convert("RGB")
r, g, b = im.split()
raw = Image.merge("RGBX", (b, g, r, Image.new("L", im.size, 255))).tobytes()
open(sys.argv[1], "wb").write(gzip.compress(raw, 9))' "$T/splash.raw.gz"
(cd "$T" && find . | cpio -o -H newc --owner=0:0 2> /dev/null | gzip -9) > "$OUT"
echo "$OUT: $(stat -c %s "$OUT") bytes"
