#!/bin/bash
# Builds the M13 boot image: copies the files of kernel/ (device tree and our drivers) into the
# kernel sources, applies kernel-patches/, configures and builds kernel and device tree, embeds
# them in uniLoader together with the ramdisk (built here, from mangos/initramfs) and assembles
# the Android image. The kernel and uniLoader sources must be at the versions pinned in common.sh
# (scripts/prepare_env.sh brings them there).
#
# The image name becomes part of the kernel version: each image gets its own module directory on
# the phone (/lib/modules/<version>), and the modules of one do not load on another.
#
# Usage: bash scripts/build_m13_boot.sh <output image, .img>
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
OUT=$(realpath -m "${1:?missing output image path}")
KERNEL=$W/linux-wifi
UNILOADER=$W/uniLoader
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

check_commit() {
	[ "$(git -C "$1" rev-parse HEAD 2> /dev/null)" = "$2" ] ||
		{ echo "the sources in $1 are not at commit $2: run scripts/prepare_env.sh" >&2; exit 1; }
}
check_commit "$KERNEL" "$KERNEL_COMMIT"
check_commit "$UNILOADER" "$UNILOADER_COMMIT"
[ -s "$A13_IMAGE" ] || { echo "missing $A13_IMAGE: run scripts/prepare_env.sh" >&2; exit 1; }
[ -s "$W/firmware/$FIRMWARE_HIMAX" ] ||
	{ echo "missing $W/firmware/$FIRMWARE_HIMAX (extract it from the original phone)" >&2; exit 1; }

RAMDISK=$(mktemp)
trap 'rm -f "$RAMDISK"' EXIT
bash "$REPO/scripts/build_initramfs.sh" "$RAMDISK"

# Hooks one of our drivers into the kernel: a line in the directory's Makefile and a configuration
# entry (from kernel-kconfig/<SYMBOL>) before the first "config <PREFIX>" entry of the Kconfig.
add_driver() {
    local dir="$1" symbol="$2" object="$3" prefix="$4"
    grep -q "CONFIG_$symbol)" "$dir/Makefile" ||
        printf 'obj-$(CONFIG_%s)\t+= %s\n' "$symbol" "$object" >> "$dir/Makefile"
    if ! grep -q "^config $symbol\$" "$dir/Kconfig"; then
        awk -v snippet="$REPO/kernel-kconfig/$symbol" -v first="^config $prefix" '
            !done && $0 ~ first { while ((getline line < snippet) > 0) print line; done = 1 }
            { print }' "$dir/Kconfig" > "$dir/Kconfig.new"
        mv "$dir/Kconfig.new" "$dir/Kconfig"
    fi
    grep -q "^config $symbol\$" "$dir/Kconfig"
}

cd "$KERNEL"
cp -r "$REPO/kernel/." .
# All fixes in kernel-patches/ are applied: one already applied is skipped, one that does not fit
# stops the build.
for patch in "$REPO"/kernel-patches/*.patch; do
    git apply --check --reverse "$patch" 2> /dev/null && continue
    echo "applying $(basename "$patch")"
    git apply "$patch"
done
grep -q m135f arch/arm64/boot/dts/exynos/Makefile ||
    sed -i 's/^\texynos850-a135f-6gb.dtb\t\t\\$/&\n\texynos850-m135f.dtb\t\t\\/' arch/arm64/boot/dts/exynos/Makefile
add_driver drivers/power/supply CHARGER_SM5714 sm5714.o CHARGER_
add_driver drivers/video/backlight BACKLIGHT_S2DPS01 s2dps01.o BACKLIGHT_
add_driver drivers/input/touchscreen TOUCHSCREEN_HIMAX_HX83112F himax_hx83112f.o TOUCHSCREEN_

# The configuration always starts from the postmarketOS one for the A13. The PMIC (regulators,
# power key, clock) goes through the ACPM protocol, and the touchscreen firmware is needed before
# the system is mounted: both are built into the kernel. The firmware is Samsung's and stays in
# $W/firmware.
cp "$REPO/kernel-kconfig/config-postmarketos-exynos850.aarch64" .config
scripts/config --enable CHARGER_SM5714 --enable BACKLIGHT_CLASS_DEVICE --enable BACKLIGHT_S2DPS01 \
    --enable TOUCHSCREEN_HIMAX_HX83112F --enable F2FS_FS \
    --enable EXYNOS_MBOX --enable EXYNOS_ACPM_PROTOCOL --enable MFD_SEC_ACPM --enable EXYNOS_ACPM_CLK \
    --enable INPUT_S2MPU12_PWRKEY \
    --enable INPUT_FF_MEMLESS --enable INPUT_REGULATOR_HAPTIC \
    --set-str EXTRA_FIRMWARE "$FIRMWARE_HIMAX" --set-str EXTRA_FIRMWARE_DIR "$W/firmware"
scripts/config --set-str LOCALVERSION "-$(basename "$OUT" .img | tr -c 'A-Za-z0-9.\n' '-')"
scripts/kconfig/merge_config.sh -m .config "$REPO/kernel-kconfig/wifi.config" > /dev/null
make olddefconfig > /dev/null
make -j"$(nproc)" Image dtbs > "$W/build.log" 2>&1 ||
    { echo "KERNEL BUILD FAILED" >&2; grep -B2 -A8 -m3 'error' "$W/build.log" >&2; exit 1; }

cd "$UNILOADER"
cp "$KERNEL/arch/arm64/boot/Image" blob/Image
cp "$KERNEL/arch/arm64/boot/dts/exynos/exynos850-m135f.dtb" blob/dtb
cp "$RAMDISK" blob/ramdisk
make ARCH=aarch64 a13_defconfig > /dev/null
make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" > "$W/uniloader.log" 2>&1 ||
    { echo "UNILOADER BUILD FAILED" >&2; tail -n 15 "$W/uniloader.log" >&2; exit 1; }

python3 "$REPO/scripts/make_bootimg.py" "$A13_IMAGE" uniLoader "$OUT"
echo "kernel $(cat "$KERNEL/include/config/kernel.release")"
