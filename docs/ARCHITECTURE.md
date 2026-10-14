# Architecture

How mangOS is built, from the bootloader up to the agent. For the work cycle see
[DEVELOPMENT.md](DEVELOPMENT.md); for rebuilding or porting see [PORTING.md](PORTING.md).

mangOS is two layers. The lower one (boot chain, device tree, drivers, firmware) is specific to the
Galaxy M13. The upper one (UI, bridge, agent, voice, `pim`) is ordinary Linux software and does not
care what phone it runs on.

1. [Boot chain](#boot-chain)
2. [Partitions and the two kernels](#partitions-and-the-two-kernels)
3. [Services](#services)
4. [Voice](#voice)
5. [Bridge and events](#bridge-and-events)
6. [The agent and `pim`](#the-agent-and-pim)
7. [Kernel modules of our own](#kernel-modules-of-our-own)
8. [Where configuration and data live on the phone](#where-configuration-and-data-live-on-the-phone)
9. [Hardware notes](#hardware-notes)
10. [Security](#security)

## Boot chain

```
Samsung bootloader (signed, kept)
  -> boot partition: Android boot image v2, whose "kernel" is uniLoader
       inside: Linux + M13 device tree + ramdisk
  -> ramdisk (mangos/initramfs/init): USB network, e2fsck, mounts the system from userdata
  -> mangOS (Alpine base, directory /mangos of the system partition)
       fallback if the previous boot did not finish: the postmarketOS (Nura) rootfs
```

- **Bootloader.** Samsung's, and its security firmware, are signed and stay. They load the image
  from `boot`, or from `recovery` when a register (`INFORM3`) asks for it.
- **Boot image.** `scripts/build_m13_boot.sh` builds it: our files from `kernel/` are copied over the
  kernel sources, `kernel-patches/` are applied, kernel and device tree are built and embedded in
  uniLoader with the ramdisk, and `scripts/make_bootimg.py` assembles the Android image. The image
  name becomes part of the kernel version, so each image has its own `/lib/modules/<version>`.
- **Device tree.** `kernel/arch/arm64/boot/dts/exynos/exynos850-m135f.dts` sits on top of the
  Galaxy A13 tree of exynos850-mainline. It also sets the boot arguments
  (`clk_ignore_unused pd_ignore_unused regulator_ignore_unused`): the bootloader leaves the audio,
  sensor and camera blocks powered and our modules rely on that.
- **Ramdisk.** `mangos/initramfs/init` creates the USB network gadget (NCM, phone at
  `172.16.42.1`), finds the `userdata` partition by name, reads the nested GPT inside it (partition
  1 boot, partition 2 system), attaches the system partition to a loop device and runs `e2fsck -p`
  (a full `-y` repair if the exit code is 4 or more). The log is `/var/log/initramfs.log`.
  Then it starts mangOS if `/mangos/bin/busybox` exists, `/mangos/.disabled` does not, and
  `/mangos/.boot-pending` is not there. Otherwise it starts the fallback system. If it cannot get
  that far (no `userdata`, unreadable partition table, loop device or mount failed) it opens a root
  shell without a password on the USB network (`nc 172.16.42.1 23`, see [Security](#security)).
- **Boot marker.** Before starting mangOS the ramdisk creates `.boot-pending`.
  `etc/local.d/25-boot-ok.start` removes it once boot reaches that point. A boot that leaves the
  marker is treated as failed and the next one starts the fallback.

## Partitions and the two kernels

| Partition | Use |
|---|---|
| `boot` | The kernel known to work |
| `recovery` | Test kernels. Started with `mangos-reboot recovery`; `mangos-reboot` returns to `boot` |
| `up_param` | The boot screens shown by the bootloader (`scripts/build_up_param.py`) |
| `userdata` | A nested GPT with two partitions: boot and system. mangOS lives in `/mangos` on the system one |
| all others | Bootloader, factory data, firmware. Read-only at boot (`05-partlabels.start`) |

`/dev/disk/by-partlabel/<name>` links are created at boot, so scripts use names, not numbers. The
module `reboot_target` writes the wanted boot target into the register the bootloader reads
(recovery, download mode, or normal), at every reboot; without it the bootloader repeats the
previous mode. The cycle that uses the two kernels is in [DEVELOPMENT.md](DEVELOPMENT.md#try-a-kernel).

## Services

OpenRC services in `mangos/rootfs/etc/init.d/`, programs in `mangos/rootfs/usr/bin/`, all
supervised and restarted if they die. A voice or transcription service whose configuration cannot
work stops after five restarts in a minute instead of restarting for ever.

| Service | Program | What it does |
|---|---|---|
| `mangos-bridge` | `mangos-bridge` | Python server on `127.0.0.1:8098`. Serves the UI, state, keys, touch, screen, GPU frequency, volume, the voice conversation and what pages ask of the phone; watches disk use |
| `mangos-web` | `mangos-web` | `cage` compositor plus a Chromium kiosk showing the UI page. Waits for the bridge and `seatd` |
| `mangos-agent` | `mangos-agent` | Keeps Claude Code running as user `pim`, one session or two, and streams its replies phrase by phrase to the bridge and the speaker |
| `mangos-parakeet` | `mangos-parakeet` | Speech to text with Parakeet, kept in memory, on `127.0.0.1:8178`; with `speech=cloud` it does not load the model |
| `mangos-speak` | `mangos-speak` | Text to speech with espeak-ng and a Piper voice kept in memory, or with Deepgram's Aura-2 when `speech=cloud`; plays with `aplay` on the ALSA `default` device. Socket `/run/mangos-speak.sock` |
| `mangos-photo` | `mangos-photo` | Camera service on `/run/mangos/photo.sock`; the command of the same name asks it for photos |
| `mangos-sound` | `mangos-sound` | The only owner of `/dev/abox-speaker`: reads the ALSA mix (`/etc/asound.conf`: `dmix` on the `snd-aloop` virtual card, loaded by its start script) and writes it to the speaker, opened while there is sound and closed after 2 s of silence. Everything that plays (Chromium, `aplay`) uses the ALSA `default` device |
| `mangos-wifi` | `wpa_supplicant` | Keeps the saved Wi-Fi network. Does not start while none is saved. Before starting it installs the factory MAC address (see [Hardware notes](#hardware-notes)) |
| `mangos-dhcp` | `udhcpc` | Address on the Wi-Fi, renewed before it runs out |
| `mangos-usbnet` | `udhcpd` | Address and DHCP for the USB network created by the ramdisk |
| `mangos-watchdog` | `busybox watchdog` | Pets `/dev/watchdog` every 10 seconds; a hung kernel reboots the phone after 60 |
| `ntpd` | `busybox ntpd` | Network time, as a client (`etc/conf.d/ntpd`) |

Commands without a service: `mangos-mic` (the microphone as a 16 kHz mono stream),
`mangos-transcribe` (audio to text), `mangos-reboot`, and `pim`. `mangos-speak` and `mangos-photo`
without their service stop with an error instead of loading a second copy of the models.

Boot scripts in `etc/local.d/`: `05-partlabels` (partition links, read-only protection), `10-clock`
(clock from the RTC, or from the time saved at shutdown, until `ntpd` has the network time),
`25-boot-ok`, and `30-hardware`, which sets transparent huge pages to `madvise` and loads the
modules that depend on Samsung firmware (`abox`, `chub`). Before loading each module it leaves a
marker file, removed when loading ends; a marker found at boot means the previous load hung the
kernel, and that module is skipped (log: `/var/log/mangos-hardware.log`). The modules in
`etc/modules-load.d/mangos.conf` load at boot through OpenRC; the camera module is loaded by
`mangos-photo` when needed.

## Voice

A tap on the microphone, or on the entity, opens a voice conversation in the bridge:

1. `mangos-mic` reads `/dev/abox-mic` and writes 16 kHz mono audio, multiplied by `gain=`.
2. `mangos-transcribe listen --send` reads it, cuts the phrase and transcribes it, then delivers
   the text to the agent through the bridge. Locally it asks `mangos-parakeet` (the phrase ends
   after 800 ms of silence); in the cloud it streams to Deepgram's Nova-3 and Deepgram says when
   the phrase is over. The UI shows the words as they arrive.
3. The agent receives the text starting with `(spoken)`, and every finished phrase of the reply
   goes to `mangos-speak` over its socket: a Piper voice on the phone, or Deepgram's Aura-2.
4. The sound goes through `aplay` into the mixer and `mangos-sound` to the speaker. When the reply
   has been spoken the bridge reopens the microphone. The conversation closes with a tap, or when
   nobody speaks.

`pim speech local|cloud` chooses where steps 2 and 3 run (`speech=` in `/etc/mangos/voice`). The
cloud needs the network and `/etc/mangos/deepgram-key`. There is no fallback from one to the
other: if the chosen one fails, the failure is reported to the person and logged. Exit codes and
options are in the headers of `mangos-transcribe` and `mangos-speak`.

## Bridge and events

The UI is one page, `mangos/web/index.html` (WebGL, no libraries to install). The bridge serves it
and talks to it through:

- `GET /events`: a Server-Sent Events stream of JSON events (`screen`, `say`, `face`, `surface`,
  `voice`, `volume`, `house`, `queued`, `off_charging`...). The last event of each state type is
  replayed to a client that connects later.
- `POST /command`: a command for the UI as JSON; this is what `pim` sends.
- `POST /user` and `GET /user`: the person's messages to the agent, typed or spoken.
- Also `/status`, `/sensors`, `/pages`, `/house`, `/voice`, `/device`, `/metrics` and
  `/camera.jpg`. The full list, with what each does, is the header of `mangos-bridge`.

The pages the agent makes run in the browser, each in a sandboxed `<iframe>` without
`allow-same-origin`. They cannot call the bridge: the bridge rejects POSTs whose `Origin` is not
the UI's, and `GET /user` requests sent by a browser. They can only ask the UI, through
`mangos/web/mangos.js`, for vibration, speech, photos and the motion sensors, and nothing asks the
person first (see [Security](#security)). The bridge also reads the keys and the touchscreen (the
power key turns the screen off and on; after two minutes without activity it turns off by itself),
sets the GPU frequency through `acpm_gpu`, reads the accelerometer and compass from the `chub`
debugfs file, and measures what takes space on the disk: above 90% it tells the agent once, with a
message that starts with `(mangOS)`. `GET /status` carries those numbers (`disk`).

## The agent and `pim`

`mangos-agent` runs Claude Code as the unprivileged user `pim` (home `/home/pim`), with `doas`
rights to become root. It authenticates with the subscription token in `/etc/mangos/claude-token`.
A session lasts: if the process dies it resumes the same conversation, and a long one is
summarized with `/compact`. Sessions start with `--system-prompt-snapshot off`, so a conversation
that is resumed follows the instructions as they are now. While the model uses tools, the agent
sends a `work` event and the laptop appears in the room. The laptop is `mangos/web/laptop.png`: 19
poses rendered from the 3D model in `branding/laptop/`; the UI shows the pose that matches how far
the entity has turned on its swivel chair.

**One agent or two**, by `agents=` in `/etc/mangos/entity` (`pim agents 1|2` switches it and
restarts `mangos-agent`):

| `agents=` | Sessions |
|---|---|
| `1` (default) | One Claude Code, role `solo` (`model=`, Haiku if missing), in `/home/pim`. It talks and does every job |
| `2` | The one that talks, role `talker` (`model=`), in `/home/pim/talker`; it cannot write files or search the web, and hands every job to the second, role `worker` (`worker_model=`, Sonnet if missing), in `/home/pim/worker`, run as a thread of the same `mangos-agent` process |

The two sessions are separate conversations that write to each other with `SendMessage`, by name
(`talker`, `worker`). Each keeps its own conversation across restarts and across switches between
1 and 2. When the worker writes back, the talker wakes by itself: that turn is shown and, if a
voice conversation is open, spoken with the microphone held closed meanwhile. To the person there
is one entity either way.

**Instructions.** `mangos/pim/CLAUDE.md` is the core, installed as `/home/pim/CLAUDE.md` and read
by every session: who the entity is, system messages (`(mangOS)`), the notebook `~/notes.md`, and
the limits. The files of `mangos/rootfs/usr/share/mangos/pim/` are the roles: `voice.md` (how to
talk and the commands of `pim`), `craft.md` (pages, the room, changing the UI), `talker.md`,
`worker.md`. `mangos-agent` composes the prompt of each role from the name and color in
`/etc/mangos/entity` and its files (`ROLES` in the program). `sessions.json` in the same folder
lets the sessions receive each other's messages. Logs: `mangos-agent.log`, and the errors of
Claude Code in `mangos-claude.log` (`solo`, `talker`) and `mangos-worker-claude.log`.

`pim` (`usr/bin/pim`, a shell script) is how the agent, or anyone on the phone, moves the screen
and the room, shows pages, takes photos, speaks, and changes the speech and agent choices. Run
`pim` without arguments for the list. Most commands are requests to the bridge; the others call
`mangos-photo`, `mangos-speak` or `grim`, or change a settings file and restart the services that
read it (`pim speech`, `pim agents`).

## Kernel modules of our own

Sources in `mangos/tools/`, built by `scripts/install_modules.sh`. Registers and sequences come from
the Samsung kernel source for this exact model.

| Module | Interface | What it does |
|---|---|---|
| `abox` | `/dev/abox-mic` (read), `/dev/abox-speaker` (write), both 48 kHz 16 bit stereo; debugfs `/sys/kernel/debug/abox/{status,log,command}` | Starts the audio processor with Samsung's firmware; microphone and speaker. `command` takes `volume N` |
| `snd-aloop` (with `soundcore`, `snd`, `snd-timer`, `snd-pcm`, from the kernel) | ALSA card `Loopback` | The virtual card the ALSA mix goes through on its way to `mangos-sound` |
| `chub` | debugfs `/sys/kernel/debug/chub/{status,log,command}` | Starts the sensor microcontroller with Samsung's firmware; accelerometer and compass. `command` takes `accel 1 [ms]`, `compass 1 [ms]`... |
| `camera` | `/dev/camera-rear`, `/dev/camera-front`; debugfs `camera-rear`, `camera-front` | Raw shots: write `exposure=`, `gain=`, `focus=`, `shoot`, `start`; read the frame. No image processor: `mangos-photo` develops the image. Needs the kernel's `s2mpb03-regulator` module (supply chip of the cameras) |
| `acpm_gpu` | `/sys/devices/platform/acpm-gpu/{khz,allowed_khz}`; cooling device `gpu` | GPU frequency, through the power-management firmware |
| `acpm_thermal` | thermal zones `chip-cpu1`, `chip-cpu2`, `chip-gpu` | Chip temperatures; lowers CPU and GPU frequencies from 76 degrees |
| `reboot_target` | the argument of `reboot` | Tells the bootloader where to restart from |

`acpm_test` (sends one request to the power-management firmware) and `ramwatch.c` (finds RAM that
something else overwrites) are test tools, not part of the system.

Drivers in `kernel/drivers/` are built into the kernel: `sm5714.c` (charger and fuel gauge, the
power supply `sm5714-battery`), `s2dps01.c` (backlight), `himax_hx83112f.c` (touchscreen; loads
its firmware at every boot). `kernel-patches/` holds fixes to the Wi-Fi driver (SCSC, four
patches), the CPU clock driver and the RTC driver. `kernel-kconfig/` holds the base configuration
and the entries for the Wi-Fi, our drivers and the rest.

## Where configuration and data live on the phone

| Path | Content |
|---|---|
| `/etc/mangos/entity` | `name=`, `color=`, `model=` (Haiku if missing), `agents=` (`1` or `2`), `worker_model=` (Sonnet if missing) |
| `/etc/mangos/voice` | `speech=local` or `cloud` (`pim speech`), `gain=` of the microphone, `voice=` (Piper), `cloud_voice=` (Deepgram), `reply_by_voice=no` to keep replies silent |
| `/etc/mangos/claude-token` | Subscription token, root only (`scripts/phone_token.sh`) |
| `/etc/mangos/deepgram-key` | Deepgram key for cloud speech, root only (`scripts/phone_deepgram.sh`) |
| `/etc/mangos/web-test` | If present, the browser opens a local debug port (`scripts/tests/ui_touch.py`) |
| `/etc/wpa_supplicant/wpa_supplicant.conf` | Saved Wi-Fi (`scripts/phone_wifi.sh`) |
| `/var/lib/mangos/` | `settings` (volume), `house.json`, `house/` (the room's objects), `surfaces/` (the pages the agent shows; `.modes.json` says how each was last shown), `photos/`, `chromium/`, `installed.md5`, `powered-off` (marks the off-charging state), `last-time` (clock saved at shutdown), `<module>-loading` (marker while a hardware module loads) |
| `/var/cache/mangos/speak` | Short phrases already synthesized, per voice |
| `/home/pim/` | `CLAUDE.md` (the core instructions), `notes.md` (the entity's notebook), `pages/` and `house/` (where it writes pages and objects before showing them), `talker/` and `worker/` (the working folders of the two roles), `.claude/` (the conversations) |
| `/usr/share/mangos/web`, `/usr/share/mangos/fonts`, `/usr/share/mangos/pim` | The UI page and `mangos.js`; fonts; the role instructions and `sessions.json` |
| `/opt/mangos/asr`, `/opt/mangos/voices` | Parakeet model and onnx-asr library; Piper voices |
| `/run/mangos-speak.sock`, `/run/mangos/photo.sock` | Sockets of the voice and camera services, for root and the `pim` group |
| `/lib/firmware/` | Samsung firmware (touchscreen, Wi-Fi, audio, sensors) and `conf/wlan/mac.txt` |
| `/var/log/` | `mangos-*.log` per service, `initramfs.log`, `mangos-hardware.log` |

## Hardware notes

What the Galaxy M13 (`SM-M135F`, Exynos 850) has and how it behaves. The values come from the
Samsung kernel source for this model and from measurements. The stories of how they were found
are in [LESSONS.md](LESSONS.md).

**Power-management firmware (ACPM).** CPU and GPU frequency and the chip temperature are set and
read by firmware, through requests the kernel sends over mailbox channels.

- Channel 0 carries the clocks: ids 2 and 3 are the two CPU clusters (130 to 1846 MHz,
  `schedutil`; the voltage follows by itself), id 4 the GPU, ids 0 and 1 memory and the internal
  bus, which the bootloader leaves at their maximum. Channel 9 carries temperature (zones 0, 1, 2).
  Channel 2 carries the power-supply protocol the audio codec uses. Channel 5, which the vendor's
  device tree gives for the clocks, is interrupt-driven and does not work for them.
- Asked "what frequency are you at", the firmware always answers with all bits set: the driver
  remembers the last frequency requested, and the first one must be requested explicitly.
- GPU frequencies: 377, 433, 598, 754, 865 and 1001 MHz. The bootloader leaves the GPU at 377;
  the WebGL UI draws about 23 frames per second there and 55 at 1001 (`acpm_gpu`, set by the
  bridge: the maximum while the screen is on).

**Memory.** The first RAM block of the device tree ends at `0xbc400000`. The A13 tree goes on to
`0xbc900000`, but on the M13 those 5 MB read back as zeros. 32 MiB of contiguous memory belong
almost entirely to the audio firmware; another 32 MiB, below 4 GiB because the address register is
32 bits wide, are reserved for the cameras. The two camera frames together are 31,516,160 bytes,
so they would fit, but the cameras are used one at a time (below). There is no swap and no `zram`
(`CONFIG_ZRAM` is not set; `CONFIG_PSI` is).

**Display and touch.** The screen is 1080 x 2408. The display controller has no mainline driver:
the UI draws into the framebuffer the bootloader sets up. The M13 ships with three touchscreen
controllers (Ilitek, Focaltech, Himax), told apart by three board pins (`gpg1-7`, `gpg2-0`,
`gpa0-2`); this phone reads 0, 0, 1: Himax HX83112F with a BOE panel (the chip answers ID
`0x83112f`). It has no flash: the driver loads its firmware at every boot, and DMA is off on its
SPI bus. The backlight is an `s2dps01` on `i2c_1`: register `0x23` is the level (0 to 255), the
driver stops at 192, Samsung's normal maximum (`/sys/class/backlight/s2dps01/brightness`).

**Power and charging.** The charger and fuel gauge are an `sm5714` on `i2c_0`, which has no external
pull-up resistors: the SoC's internal ones are enabled in the device tree. The battery is
`/sys/class/power_supply/sm5714-battery`; `charge_control_end_threshold` stops charging at the
given percentage and resumes 5 points below, but it starts at 100 at every boot and nothing sets
it. The phone charges at 500 mA. The PMIC does not cut power while a charger is connected, so
`pim poweroff` with the cable in reboots into a charging screen (`off_charging`); without the
cable the phone powers off. The PMIC's "auto reboot after sudden power loss" bit (`PWRONSRC`
register `0x0e`, value `0x80`) must be cleared at shutdown by the RTC driver
(`kernel-patches/rtc-s5m-shutdown.patch`).

**Wi-Fi.** Samsung SCSC ("Maxwell"), part of the Exynos 850. The vendor firmware comes from the
`vendor` partition and lives in `/lib/firmware`. The driver reads the MAC address from
`/lib/firmware/conf/wlan/mac.txt`; the factory one is in the read-only `efs` partition
(`wifi/.mac.cob`, 17 characters), which `mangos-wifi` mounts `-o ro,noload` to copy it. The USB
gadget has fixed locally administered addresses (`02:4d:61:6e:67:01` phone, `:02` computer).

**Audio.** A processor of its own (ABOX 2.30) with firmware (`calliope_*.bin`, unsigned) and its
own sysmmu and interrupt controller. The microphones are analog, on the `aud3004x` codec inside
the power chip (MIC1 is the main one); the two channels of `/dev/abox-mic` are not equal and
`mangos-mic` uses the left. The speaker amplifier is on an I2C bus of its own with only internal
pull-ups. The firmware also touches areas it does not need (memory shared with the modem at
`0xA0000000`, a register at `0xE0000000`): anything not mapped for it must point to one spare page,
or the sysmmu stops. The interrupt controller of the ABOX is reached through the secure firmware,
not directly.

**Sensors.** The accelerometer (`lis2dlc12`) and the compass (`mmc5633`) sit behind a
microcontroller in the chip (CHUB) with firmware of its own (`shub_nacho_*.bin`). There is no
gyroscope and no light sensor; proximity comes from the touchscreen.

**Cameras.** Rear `s5kjn1` (50 MP, `hsi2c_1`, address `0x2d`, receiver CSIS1, focus motor DW9808 on
`hsi2c_0`, address `0x0c`) and front `gc08a3` (8 MP, `hsi2c_2`, address `0x31`, receiver CSIS0).
There is no image processor, so `mangos-photo` develops the raw data. Only one camera works at a
time. The supply chip is a dedicated `S2MPB03`. The sensor clocks (26 MHz) are missing from the
kernel's clock driver, but the bootloader leaves them routed.

**Inventory.** What else the phone has, and what is known about it.

| Component | What the phone has | Notes |
|---|---|---|
| Graphics | Mali-G52 | `panfrost`; a devfreq table in the device tree would let it scale by itself |
| Display | DECON controller, DSI panel | No mainline driver for the Exynos 850 |
| Body-proximity sensor | `sx938x` on `i2c_4`, address `0x28` | Related drivers (`sx9310`, `sx9324`) are in the kernel |
| Vibration | Motor behind a regulator | `regulator-haptic` driver, in the kernel; on or off, no intensity |
| Flashlight | Inside the `sm5714` | Would be added to our driver |
| Bluetooth | Same chip as the Wi-Fi | Not started |
| GPS | Samsung receiver with firmware | Not started |
| Fingerprint reader | Goodix | No open driver known |
| USB host | `dwc3` controller | Works as a device only; the `sm5714` would have to supply power |
| Fast charging | `sm5714` (USB-PD) and AFC | Fixed at 500 mA |
| Suspend | Handled by firmware | The kernel offers it; never tried |
| CPU idle states | Declared by Samsung | None active |
| Memory and bus frequencies | `mif`, `int`, `disp`, `cam`, `aud` clocks | Stay where the bootloader leaves them |
| Watchdog | Two in the chip | In use |
| SD card | Second controller | Not started |
| Modem | Samsung, integrated | Not done yet (calls, SMS) |

## Security

mangOS gives its agent full control of a phone it trusts, by design. What that exposes:

- **Network.** `sshd` listens on every interface (USB and Wi-Fi), as `root`, with a single public
  key (the one given to `build_mangos_rootfs.sh`). There is no firewall. `root` has an empty
  password and no login terminal exists on the screen, so access depends on `sshd`'s default of
  refusing empty passwords: do not change it. The bridge and `mangos-parakeet` listen on
  `127.0.0.1` only; the voice and camera sockets belong to `root` and the `pim` group.
- **The agent is root.** `pim` has `doas` without a password and Claude Code runs with its
  permission prompts off. Anything the agent reads (web pages, files, messages) can steer it.
  Its instructions (`mangos/pim/CLAUDE.md`) set limits, but they are instructions: only the
  read-only protection of the factory partitions is enforced.
- **Secrets.** `/etc/mangos/claude-token` and `/etc/mangos/deepgram-key` are readable by root
  only, but the agent can become root, and the Claude token is also in the environment of the
  Claude Code process, so every command the agent runs inherits it. Use keys made for this phone
  that you can revoke.
- **The browser.** Chromium runs as root with `--no-sandbox` (the compositor owns the screen) and
  shows pages the agent writes, which may load images and scripts from the Internet. A renderer
  exploit would give root. Safe Browsing and component updates are off.
- **What pages can do.** Pages are isolated from the bridge (see above), but through `mangos.js`
  they can make the phone vibrate, speak aloud at the volume in use, take photos with either
  camera and stream the motion sensors. Nothing asks the person first; only the agent's
  instructions say "when asked".
- **Rescue shell.** If the ramdisk fails before it can start a system, anyone with the USB cable
  has a root shell without a password and access to all of `userdata`. It is how a dead phone is
  recovered (`scripts/phone_sh.sh`, `scripts/push_image.py`). The USB identifier the ramdisk
  presents is Google's (`18d1:d001`), so a PC may apply Android rules to it.
- **Debug port.** While `/etc/mangos/web-test` exists the browser listens on `127.0.0.1:9222`:
  anything on the phone, the agent included, controls the browser as root. Remove the file and
  restart `mangos-web` when the test is done.
- **Updates.** Nothing updates the packages or the kernel by itself. `apk upgrade` works as on any
  Alpine; the kernel is fixed at the commit in `scripts/common.sh`.
- **What leaves the phone** is described in the [README](../README.md#privacy).
