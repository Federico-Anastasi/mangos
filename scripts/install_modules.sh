#!/bin/bash
# Compiles kernel modules for the kernel running on the phone and installs them while the phone is
# on, without touching the boot partition: it is the way to try a driver without risking a kernel
# that does not boot. Modules end up in /lib/modules/<version>/mangos and are loaded with
# modprobe; those listed in mangos/rootfs/etc/modules-load.d/mangos.conf (installed by
# install_mangos.sh) start at every boot.
#
# The sources in $W/linux-wifi must be those of the running kernel: the script compares the
# version and stops if it differs. A module built from different sources may load anyway and hang
# the phone. With --next the modules are for the kernel just built and not yet booted: they are
# installed in its directory, ready for when it starts. With --into <dir> the phone is not
# touched: the modules, for the kernel of those sources, end up in <dir>/lib/modules/<version>/mangos
# (used by build_mangos_rootfs.sh).
#
# Usage, from WSL: bash scripts/install_modules.sh [--next | --into <dir>] <module>...
# A module is either a kernel driver, written SYMBOL=path/to/module.ko, or a repo directory
# holding one of our modules and its Makefile.
# Example: bash scripts/install_modules.sh REGULATOR_S2MPB03=drivers/regulator/s2mpb03-regulator.ko \
#              mangos/tools/acpm_thermal
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
NEXT=
INTO=
[ "${1:-}" = --next ] && { NEXT=1; shift; }
[ "${1:-}" = --into ] && { INTO=$(realpath -m "${2:?missing directory}"); shift 2; }
[ $# -gt 0 ] || { sed -n '2,/^[^#]/{/^[^#]/!p}' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
cd "$W/linux-wifi"

VERSION=$(cat include/config/kernel.release)
[ -n "$INTO" ] || RUNNING=$(ssh -n m13 uname -r)
[ -n "$NEXT$INTO" ] || [ "$VERSION" = "$RUNNING" ] ||
	{ echo "the sources are $VERSION, the phone runs $RUNNING" >&2; exit 1; }

FROM_KERNEL=()
OURS=()
for spec in "$@"; do
	if [ -d "$REPO/$spec" ]; then
		OURS+=("$spec")
	else
		scripts/config --module "${spec%%=*}"
		FROM_KERNEL+=("${spec#*=}")
	fi
done
BUILT=()
: > "$W/modules.log"
build_failed() { echo "MODULE BUILD FAILED" >&2; grep -B2 -A8 -m3 -i 'error' "$W/modules.log" >&2; exit 1; }
# Without the kernel symbol list (Module.symvers) unresolved symbols are only warnings: the phone
# checks them at load time.
if [ ${#FROM_KERNEL[@]} -gt 0 ]; then
	make olddefconfig > /dev/null
	KBUILD_MODPOST_WARN=1 make -j"$(nproc)" "${FROM_KERNEL[@]}" >> "$W/modules.log" 2>&1 || build_failed
	BUILT+=("${FROM_KERNEL[@]}")
fi
# Our modules are built in a copy outside the repo, which stays free of build files.
for dir in "${OURS[@]}"; do
	copy="$W/modules/$(basename "$dir")"
	rm -rf "$copy" && mkdir -p "$copy"
	cp -r "$REPO/$dir/." "$copy/"
	KBUILD_MODPOST_WARN=1 make -j"$(nproc)" M="$copy" modules >> "$W/modules.log" 2>&1 || build_failed
	BUILT+=("$copy"/*.ko)
done

if [ -n "$INTO" ]; then
	mkdir -p "$INTO/lib/modules/$VERSION/mangos"
	cp "${BUILT[@]}" "$INTO/lib/modules/$VERSION/mangos/"
	# As on the phone, depmod warns that the kernel's own module lists are missing: expected.
	depmod -b "$INTO" "$VERSION" 2> /dev/null
	ls -l "$INTO/lib/modules/$VERSION/mangos"
	exit 0
fi
ssh -n m13 "mkdir -p /lib/modules/$VERSION/mangos"
scp -q -l 80000 "${BUILT[@]}" "m13:/lib/modules/$VERSION/mangos/"
# depmod warns that the kernel's own module lists are missing: expected, the kernel has none.
ssh -n m13 "depmod $VERSION 2> /dev/null; ls -l /lib/modules/$VERSION/mangos"
