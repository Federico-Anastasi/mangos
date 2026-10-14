# Porting mangOS

Two guides in one. Part A rebuilds mangOS on the same phone, a Samsung Galaxy M13 (`SM-M135F`).
Part B is for another phone: what is specific to the M13, and what to look for on yours.

Read [LESSONS.md](LESSONS.md) too. It is the list of problems we hit, and most of them will
happen again on any phone. [ARCHITECTURE.md](ARCHITECTURE.md) describes how the system is built,
and [DEVELOPMENT.md](DEVELOPMENT.md) has the work cycle, what to do when something goes wrong and
the safety rules; read the rules before you write anything to the phone.

**You can make the phone unusable.** Unlocking the bootloader wipes it and trips the Knox flag, and
a wrong write can leave it dead. There is no way back to Android without the full original
firmware: dump it first (step 2). There is no warranty.

**Honest caveats.** The steps below are the ones the author used, and the scripts are the ones
that built the running system. But the from-scratch system builder (`build_mangos_rootfs.sh`)
has never been run end to end on an empty phone, and `push_image.py` has been used only with the
postmarketOS ramdisk, not with ours. Both are marked below. Expect to debug.

## Part A. The same phone: Galaxy M13 `SM-M135F`

### What you need

- A Galaxy M13 `SM-M135F`. The board used here is `M13 EUR OPEN REV02`, with a Himax touchscreen
  controller and a BOE panel. The M13 exists with other touchscreen controllers: see Part B.
- A PC with Windows, WSL2 with Ubuntu 24.04, and
  [usbipd-win](https://github.com/dorssel/usbipd-win) to pass the USB port to WSL. A native Ubuntu
  24.04 PC should work without `usbipd-win`, but it has not been tried.
  Plenty of free disk (kernel tree, firmware, a system archive of more than 1.5 GB). The optional
  Parakeet encoder rebuild (`scripts/parakeet_mixed.py`) needs 6 GB of memory.
- A USB cable. The phone is reached at `172.16.42.1` over the USB network, and later over Wi-Fi.
- A Claude subscription and its token (`claude setup-token`), for the agent. Check Anthropic's
  current terms for your plan before you leave an agent running all the time.
- Samsung firmware extracted from your own phone (step 2). It is proprietary and is never
  distributed with mangOS.

Everything runs from WSL, from the repo folder. The working folder on the PC is `~/m13`
(variable `W`), outside the repo. Versions and checksums of everything downloaded are pinned in
`scripts/common.sh`.

### Steps

**1. Prepare the PC.** From WSL, as a normal user:

```
bash scripts/prepare_env.sh
```

It installs the packages, fetches the kernel and uniLoader sources at the pinned commits,
downloads the reference A13 boot image, the Alpine base and the ramdisk programs, builds
`heimdall`, and writes the `m13` alias into `~/.ssh/config`. It can be run again. At the end it
lists the firmware files still missing.

Check: it ends with `environment ready`.

**2. Extract the Samsung firmware** from the original phone into `~/m13/firmware`, before you
erase anything. Four groups are needed:

| File | What it is for | Where it comes from |
|---|---|---|
| `himax/hx83112f_boe_m13_f13.bin` | Touchscreen (it has no flash: the driver loads this at every boot) | Samsung's open-source kernel release for the `SM-M135F` |
| `wifi/vendor-wifi.tar` | Wi-Fi firmware, as a tar of the files in `/lib/firmware` | the phone's `vendor` partition |
| `audio/calliope_sram.bin`, `calliope_dram.bin`, `calliope_sram_2.bin`, `calliope_dram_2.bin` | Firmware of the audio processor | the `vendor` partition |
| `sensors/shub_nacho_bl.bin`, `shub_nacho_m13.bin` | Firmware of the sensor microcontroller | `firmware/sensorhub/` in Samsung's kernel release, or `vendor` |

The `vendor` partition is `f2fs`, which WSL cannot read: mount it on the phone and copy the files
out. Samsung publishes the kernel source for each model at
[opensource.samsung.com](https://opensource.samsung.com).

Also save the original partitions you may want back: `recovery`, `dtbo`, `up_param`, `vendor`,
and the partition table. The repo does not script how to dump them.

**3. Unlock the bootloader** (Samsung's OEM unlock, in the developer options and then the
bootloader menu; there is no script). It wipes the phone and trips the Knox flag. Then enter
download mode (see [DEVELOPMENT.md](DEVELOPMENT.md#if-something-goes-wrong)).

**4. Do not trust the memory map.** The device tree in `kernel/` is for the specific phone the
project used. After the first boot (step 5), before installing anything else, run `ramwatch`
(compile it with `aarch64-linux-gnu-gcc -O2 -static`) over almost all the RAM:
`ramwatch <megabytes> <seconds>`. It prints the physical address of any page that changes under
it. See [LESSONS.md](LESSONS.md), "Memory and storage". Which touchscreen controller you have is
read from three board pins; see Part B.

**5. Build and flash the first boot image.**

```
bash scripts/build_m13_boot.sh ~/m13/m13-boot/first.img
```

This builds the ramdisk itself (`build_initramfs.sh`), copies `kernel/` into the kernel sources,
applies `kernel-patches/`, builds the kernel and the device tree, embeds them in uniLoader and
assembles the Android image (`make_bootimg.py`). The image name becomes part of the kernel
version. The image contains the Samsung touchscreen firmware: do not publish it.

Put the phone in download mode, pass the USB port to WSL (see
[DEVELOPMENT.md](DEVELOPMENT.md#if-something-goes-wrong)) and, as root:

```
~/m13/heimdall-grimler/build/bin/heimdall flash --BOOT ~/m13/m13-boot/first.img
```

Check: the phone leaves the Samsung screen and shows a Linux boot. If no system is found, the
ramdisk opens a root shell without a password on the USB network: `nc 172.16.42.1 23`
(`scripts/phone_sh.sh` runs commands there).

**6. The system partition.** mangOS lives in `/mangos` on a system partition inside `userdata`,
which holds a nested GPT with two partitions (boot and system). The layout comes from the
postmarketOS (now Nura) image published for the A13: the `rootfs` image of the same release as
the reference boot image `prepare_env.sh` downloads (you download it yourself). Write it
with:

```
python3 scripts/push_image.py <rootfs image> userdata
```

with the phone stopped in the rescue shell and a tmpfs mounted on `/mt`. The script reads every
block back from the partition and compares it. This is also the fallback system the ramdisk boots
if a mangOS boot does not finish. *Not tried with our ramdisk, only with the postmarketOS one.*

**7. Install mangOS.** Two ways:

- *From scratch*: `wsl -u root bash scripts/build_mangos_rootfs.sh <your SSH public key>
  <archive.tar.gz>`. It needs the kernel already built (our modules are compiled for that
  version). It produces an archive of more than 1.5 GB, to extract into `/mangos` of the system
  partition (copy the host keys `/etc/ssh/ssh_host_*` first). The archive contains Samsung
  firmware: do not publish it. *Never run end to end on an empty phone.*
- *On a system that already boots* (the fallback, or mangOS itself):
  `bash scripts/install_mangos.sh`. It copies the files that differ, installs the packages,
  Claude Code, the `pim` user, the Piper voice and the Parakeet model, and restarts the services
  it touched. `--overwrite` replaces edits PIM made on the phone; `--into <dir>` writes into a
  folder instead.

**8. Hardware firmware and modules.** Needed on a system that was not built by
`build_mangos_rootfs.sh`:

```
bash scripts/install_hardware.sh
```

Copies the audio and sensor firmware to the phone and installs the modules mangOS needs for the
running kernel: `abox`, `chub`, the sound loopback of `mangos-sound` and the camera's regulator.
`abox` and `chub` start at the next reboot, or at once with `ssh m13 "rmmod abox; modprobe abox"`.
The rest (`reboot_target`, `acpm_gpu`, `acpm_thermal`, `camera`) come with
`build_mangos_rootfs.sh`, or by hand with `scripts/install_modules.sh`.

**9. Wi-Fi and subscription.**

```
bash scripts/phone_wifi.sh ["network name"]     # lists networks, asks the password
bash scripts/phone_token.sh                     # paste the output of `claude setup-token`
```

Both type secrets in your terminal; nothing is stored on the PC. After the Wi-Fi is up,
`ssh m13` works with and without the cable (`scripts/phone_connect.sh` looks for the cable, then
for the Wi-Fi address). For cloud speech, `bash scripts/phone_deepgram.sh` then
`ssh m13 "pim speech cloud"`.

**10. The second kernel.** With mangOS running, write the same image to `recovery`:
`bash scripts/flash_boot.sh <image> recovery`. From there every new kernel follows
[DEVELOPMENT.md](DEVELOPMENT.md#try-a-kernel).

**11. Check everything.** [DEVELOPMENT.md](DEVELOPMENT.md#verify-after-an-installation) lists what
to look at. In short: `ssh m13 rc-status` shows every `mangos-*` service as started, plus `ntpd`,
`sshd` and `local`; `ssh m13 pim status` returns JSON; `bash scripts/phone_look.sh screen.png`
takes a picture of the real screen; `/var/log/initramfs.log` has the `e2fsck` line; and the stability
tests run with memory full.

## Part B. Another phone

mangOS is two layers. The upper one (UI, bridge, agent, voice, `pim`) is ordinary Linux
software: Alpine, Chromium, Python, a web page. It does not care what phone it runs on, as long
as the kernel gives it a screen, touch, Wi-Fi and a way in. The lower one (boot chain, device
tree, drivers, firmware) is what is specific to the M13. The facts about the M13's hardware are in
the [hardware notes](ARCHITECTURE.md#hardware-notes).

### What is specific to the M13

| Area | What we did on the M13 | What to find on your phone |
|---|---|---|
| Chip | Exynos 850, which [exynos850-mainline](https://github.com/exynos850-mainline) boots on the Galaxy A13 | A mainline or close-to-mainline tree for your SoC. If there is none, this is a different and much larger project |
| Boot | Samsung bootloader, Android boot image v2, kernel slot filled by uniLoader, which holds kernel, device tree and ramdisk | How your bootloader loads an image: format, addresses, signatures, which partitions you may write. `scripts/make_bootimg.py` reuses the header of a known-good image |
| Partitions | We write only `boot`, `recovery`, `up_param`, `userdata`. The others (bootloader, factory data) are made read-only at boot (`05-partlabels.start`) | Your partition table (for Samsung, the PIT). Decide which partitions are safe to write before the first write |
| Recovery mode | `recovery` is a second kernel slot. A kernel register (`INFORM3`) tells the bootloader where to restart | The register and values your bootloader reads, **from the vendor source for your exact model**. Values from a similar model were wrong here |
| Device tree | `kernel/arch/arm64/boot/dts/exynos/exynos850-m135f.dts`, on top of the A13 one | Your board's overlay from the vendor kernel source |
| Memory map | RAM declared by the A13 tree was 5 MB larger than the real RAM on the M13 | Never copy it. Run `mangos/tools/ramwatch.c` over almost all the RAM |
| Touchscreen | Himax HX83112F, firmware loaded at every boot, DMA off on its SPI bus | The controller (read it from board pins or the vendor tree), the vendor driver sample code, the firmware |
| Charger, battery, backlight | SM5714 (our `sm5714.c`), S2DPS01 (our `s2dps01.c`), pull-ups on `i2c_0` | The PMIC and charger in your vendor tree. Look for drivers already in mainline first |
| CPU, GPU, temperature | ACPM firmware requests (channel 0 clocks, channel 9 thermal); the bootloader leaves the GPU at its lowest clock | The power-management firmware interface of your SoC, and what the bootloader leaves set |
| Audio, sensors, cameras | Our modules start the original Samsung firmware of the audio processor and of the sensor microcontroller; the cameras use their own receiver code | Whether your phone has a processor with its own firmware. Many do |
| Wi-Fi | Samsung SCSC, with the vendor firmware and four small patches | The Wi-Fi chip, in-tree driver or out-of-tree, firmware path, factory MAC address location |
| Firmware | Samsung proprietary, extracted from the phone | Same: extract from your own phone, never redistribute |

### Where to look for your phone's equivalents

- The vendor's published kernel source for your exact model. It has the device tree, the
  drivers, the register tables and often the firmware. Samsung publishes it at
  [opensource.samsung.com](https://opensource.samsung.com).
- The original device tree on the phone: the board overlay (`dtbo` partition) and the main device
  tree, decompiled with `dtc -I dtb -O dts`. The overlay shows what to look for: pins, buses,
  regulators, the touchscreen variants. The memory map and the frequency tables are in the SoC
  part of the vendor's device tree.
- Other phones with the same SoC, and the mainline project for it. In our case the A13 gave us
  the boot chain and the base device tree.
- The postmarketOS (now Nura) device packages for similar phones: `deviceinfo` gives flash method,
  offsets and boot image header version, and the kernel package gives the tree, the commit and the
  configuration a working port uses. For the A13 they are `device-samsung-a13`,
  `linux-postmarketos-exynos850` and `firmware-samsung-a13` (a recipe only: the firmware it names
  is proprietary).
- The kernel mailing lists, with the chip name and the subsystem together. The CPU frequency
  driver and the thermal driver came from there.

### Advice from the method

1. **Search before you write.** Someone has probably hit your problem. But look at the vendor's
   source tree listing too: the focus motor driver was not found by web search, and was already in
   the list of files we had downloaded.
2. **Specification before driver.** Write down what the hardware needs from the vendor source,
   with file and line. Registers and bootloader values come from there, never from analogy with a
   nearby value.
3. **One change at a time.** A test kernel that hangs costs a trip to download mode.
4. **Build the way back before the first modified kernel.** `recovery` for test kernels, a
   script that refuses to write `boot` with an untried image, download mode as the last resort.
5. **Try drivers as modules**, loaded by hand, with the watchdog running. A module enters the
   boot list only after it worked.
6. **Check memory before anything else**, then the file system, then the rest. Test with memory
   full.
7. **First read of a register you have never read** goes in a throwaway module, announced in the
   log first. Reading a block that is powered off can hang the kernel.
8. **Measure before you blame.** A speed limit is attributed to a cause only after measuring it.
9. **Write each problem down**, including the dead ends.
