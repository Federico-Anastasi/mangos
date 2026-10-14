# Lessons

Problems we hit bringing Linux to a Samsung Galaxy M13 (Exynos 850) and running an AI agent on it,
written for the next person who ports Linux to a phone. This is the only register of problems of
the project. Each entry has the same shape: **Symptom**, **Cause**, **How it was found**,
**Check first**. Wrong turns are included on purpose. An entry marked (open) is understood in part
or accepted as it is.

The specific names (registers, partition names, file names) are the M13's. The patterns are not.
How the system is built is in [ARCHITECTURE.md](ARCHITECTURE.md); the rules that came out of these
problems are in [DEVELOPMENT.md](DEVELOPMENT.md); the port guide is [PORTING.md](PORTING.md).

1. [Boot and kernel](#boot-and-kernel)
2. [Memory and storage](#memory-and-storage)
3. [Network](#network)
4. [Screen and touch](#screen-and-touch)
5. [Audio](#audio)
6. [Cameras](#cameras)
7. [Sensors](#sensors)
8. [Power, frequencies and temperature](#power-frequencies-and-temperature)
9. [Voice and models](#voice-and-models)
10. [The agent](#the-agent)
11. [Tools and method](#tools-and-method)

## Boot and kernel

### A test kernel that does not boot bricks the phone until you reach download mode

- **Symptom**: after writing a new image to `boot`, the phone stays at the boot screen on every
  power-on and cannot be reached from the PC.
- **Cause**: `boot` holds a single kernel. A fallback system protects the system, not the kernel.
  The image that failed differed from the working one only by two diagnostic prints that read
  queue pointers in the power-management driver.
- **How it was found**: by bisecting images. The way out was Heimdall in download mode with the
  last good image.
- **Check first**: before the first modified kernel, build the way back. Use a second partition
  (`recovery`) for test kernels, keep the good kernel in `boot`, and have a script that refuses to
  write `boot` with an image that has not already booted from `recovery`. Do not add diagnostic
  reads to a driver that runs at boot: try them from a module loaded on a running system.
  `kexec` did not work on this phone (the kernel loads, and after `kexec -e` the phone returns
  to the installed one), so it is not a shortcut.

### The bootloader accepts your own image in `recovery`, and remembers the last mode

- **Symptom**: a reboot lands in the same partition as the previous one. A hung test kernel in
  `recovery` comes back after a forced reboot.
- **Cause**: the bootloader reads a mode from a chip register (`INFORM3` on this phone) and
  keeps its own record of the last mode in a partition that survives forced reboots.
- **How it was found**: the first value found online (for another Exynos) was silently ignored,
  and we concluded that the mechanism did not work. The right values were in the vendor's kernel
  source for this exact model (`drivers/samsung/sec_reboot.c`): `0x12345604` recovery,
  `0x12345601` download mode, `0x12345600` normal.
- **Did not help**: writing "normal" into the register before a forced reboot or a watchdog
  reboot (both clear the registers); asking for the charging mode hoping for a single boot from
  `boot` (it boots `boot`, and the bootloader remembers that too); the key combination for
  `recovery` (it booted `boot`; never confirmed).
- **Check first**: take bootloader values from the vendor source for your model, not from a similar
  phone. Make every reboot write the mode explicitly (our `reboot_target` module does). Accept that
  a hung test kernel in `recovery` can only be removed from download mode.

### Ubuntu's `heimdall` does not write

- **Symptom**: writing `boot` in download mode seems to work, and after the reboot the old image is
  still there.
- **Cause**: `heimdall` 2.0.2 from the Ubuntu packages reports success without writing.
- **Check first**: use a build of the grimler fork (`scripts/prepare_env.sh` makes it). One operation
  per session: after a read or an error, reboot the phone.

### A boot image built with a stale ramdisk

- **Symptom**: the kernel boots, but `/var/log/initramfs.log` has no `e2fsck` line: the file system
  is not checked.
- **Cause**: the builder took whatever ramdisk was lying around, an old one, or the postmarketOS one.
- **How it was found**: by chance, rereading the script of an earlier image.
- **Check first**: the image builder must build the ramdisk itself and accept no other. After the
  first boot of every image, look for the `e2fsck` line.

### Loading a module that can hang the kernel must not become a reboot loop

- **Symptom**: with a watchdog running, a module that hangs the kernel at boot would reboot the
  phone forever.
- **Cause**: the watchdog restarts, the boot loads the module again.
- **Fix**: before loading a risky module, the boot script leaves a marker file; it removes it when
  the load ends. If the marker is there at boot, that module is skipped and the reason is logged
  (`/var/log/mangos-hardware.log`). The ramdisk's "boot not finished" marker is cleared before the
  load, so the watchdog reboot starts mangOS without that module, not the fallback system.
- **Check first**: a module goes in the load-at-boot list only after it worked.

### `poweroff` reboots instead of powering off

- **Symptom**: with no cable, the phone switches off and on again. With the cable in, it hangs
  with the screen on.
- **Cause**: two. (1) The PMIC's "auto reboot after sudden power loss" bit was set, and the RTC
  driver's shutdown function that clears it was not registered. A one-line patch fixes it
  (`kernel-patches/rtc-s5m-shutdown.patch`). (2) With a charger connected the PMIC does not cut
  power. Samsung reboots on purpose in that case, and shows the charging battery.
- **How it was found**: reading the PMIC's power-on-source register after a restart (it said
  "auto reboot"), then watching that unplugging the cable switched the phone off at once.
- **Fix**: besides the patch, the bridge leaves a marker file at an intended power off: a boot that
  finds the marker with the cable in is "charging while off", and shows only the battery.
- **Check first**: test power off both with and without the cable, and read the power-on-source
  register. Look at the cable first (`/sys/class/power_supply/sm5714-usb/online`): the phone
  answers over Wi-Fi, and with no cable a powered-off phone is started again only by the key.

### A running phone that stops answering

- **Symptom**: it answers ping but SSH never completes.
- **Cause**: seen twice: the system stuck after tests with memory full on the RAM defect (below),
  and a module hung halfway through loading after a read of a register that does not exist
  (`reboot` then never finishes).
- **Check first**: the way out is in [DEVELOPMENT.md](DEVELOPMENT.md#if-something-goes-wrong).

## Memory and storage

### RAM declared larger than the real RAM

- **Symptom**: unrelated programs die at random (browser, compositor, `apk`, commands over SSH).
  Freshly installed libraries give "Exec format error". The same file read several times has
  different checksums. A freshly written file has a block of zeros. The kernel log shows nothing.
  Everything works while memory is free; it appears when memory fills up.
- **Cause**: the device tree copied from a similar phone declared 5 MB more RAM than the phone
  has. Anything written there reads back as zeros. The kernel uses the last pages last, so only
  a full memory, and the file data passing through those pages, showed it.
- **How it was found**: after hours on wrong leads (USB traffic, Wi-Fi, GPU, large pages, the
  loop device, eMMC speed), with a program that locks most of the RAM, rereads it
  continuously and prints the physical address of the pages that change (`mangos/tools/ramwatch.c`).
  In one second it named 1280 pages, always the same.
- **Check first**: never copy a memory map from a similar model. Right after the first boot, before
  installing anything, run `ramwatch` over almost all the RAM. Run stability tests with memory full:
  with free memory this defect never showed. An error in a program is not necessarily about that
  program.

### File system with errors at every boot

- **Symptom**: `EXT4-fs: warning: mounting fs with errors` in the kernel log at every boot.
- **Cause**: errors left by the memory defect and by forced reboots; the ramdisk mounted the
  system without checking it.
- **How it was found**: by chance, reading the kernel log during an image test. The boot has no
  text on the screen, so nobody saw it.
- **Check first**: run `e2fsck -p` in the ramdisk before mounting (and `-y` if errors remain) from
  day one. After a forced reboot, look at `dmesg | grep -i ext4`.

### Memory runs out and nobody reboots the phone (open)

- **Symptom**: the phone answers ping on the cable but SSH does not reach its greeting, the Wi-Fi
  is mute and the screen stays off. It does not recover by itself in ten minutes.
- **Cause**: a C++ build with eight jobs on the phone with about 2 GB free. With no swap the
  kernel keeps dropping and reloading the pages of programs and never gets to kill a process; the
  out-of-memory killer did not act. The watchdog did not either: `busybox watchdog` is tiny, stays
  in memory and goes on being fed while everything else is stuck.
- **Fix used**: forced reboot. The file system was clean afterwards.
- **Not done**: the kernel has no `zram` (`CONFIG_ZRAM` unset) but has pressure stall information
  (`CONFIG_PSI=y`). Options: `zram` in a new image, as phones do; a guard that reads
  `/proc/pressure/memory` and kills the biggest process; a watchdog that stops being fed under
  pressure. The agent can cause the same with a heavy command.
- **Check first**: build with at most two jobs and watch `free -m`; build big programs on the PC.

## Network

### Wi-Fi does not start

- **Symptom**: `invalid size property in 'wifibt-if' node`, `Error getting mem resource for
  MAILBOX_WLBT`, then a kernel error when the chip starts.
- **Cause**: sizes in the shared device tree were written with one cell where this board uses two;
  one driver log option (`SCSC_MXLOGGER`) writes outside the mapped area and fails the kernel.
- **Fix**: correct the cells in the device tree, patch the driver (`kernel-patches/scsc-*.patch`),
  turn the option off.

### The Wi-Fi address changes at every boot

- **Symptom**: the router gives the phone a new address each boot.
- **Cause**: the driver used a fallback MAC address because it could not find the factory one.
- **How it was found**: `dmesg` lists the places the driver looks for it:
  `Direct firmware load for ./conf/wlan/mac.txt failed`, then a path the kernel refuses because it
  contains `..`.
- **Fix**: the factory MAC lives in a read-only factory partition. The Wi-Fi service copies it
  once to `/lib/firmware/conf/wlan/mac.txt`, mounting the partition `-o ro,noload` so the journal is
  not touched. Setting it by hand with `ip link set wlan0 address` also works.

### A Wi-Fi script that did not connect

- **Cause**: two. `wpa_passphrase` writes nothing when it reads the password from something that
  is not a terminal (it must be an argument); `udhcpc -q` exits after taking the address and never
  renews it.
- **Check first**: run the daemon that keeps the lease in the background (`udhcpc` without `-q`).

### Local services that fail to bind: `lo` comes up too late

- **Symptom**: `bind [127.0.0.1]: Address not available`.
- **Cause**: the loopback interface was brought up by a late boot script, after the services that
  need it.
- **Fix**: Alpine's `loopback` service in the `boot` runlevel.
- **Check first**: what several services need is switched on by a service that starts before
  them, not by a script that starts after.

### A daemon started once from a script is never supervised

- **Symptom**: if `wpa_supplicant` or `udhcpc` dies, the Wi-Fi never comes back, and without the
  cable it is the only way to reach the phone.
- **Fix**: two OpenRC services (`mangos-wifi`, `mangos-dhcp`) under `supervise-daemon`, like the
  others.
- **Check first**: everything the reachability of the phone depends on runs under a supervisor.

### The clock restarts from an old date

- **Symptom**: no network time; a reset clock starts the system in the past.
- **Fix**: the boot takes the time from the RTC if it says a plausible year, otherwise from the
  time saved at shutdown, and `ntpd` corrects it once the Wi-Fi is up.

### `dmesg` full of Wi-Fi messages

- **Symptom**: eight lines a second, pushing out everything else.
- **Cause**: two debug leftovers of the driver. An error-level print in the interrupt routine, at
  every Wi-Fi interrupt; and a "packet dropped" message for a normal result: the original checked
  `GRO_DROP`, which no longer exists in today's kernels, and the port replaced it with
  `GRO_CONSUMED`; the other branch reported multicast packets of the home network that no program
  listens to.
- **Fix**: `kernel-patches/scsc-no-irq-print.patch` and `scsc-gro-not-a-drop.patch`.

### Driver warnings when the Wi-Fi reconnects (open)

- `WARNING net/wireless/nl80211.c ... __cfg80211_alloc_event_skb` and `Unhandled Ind/Cfm: 0x2316`
  in the kernel log at reconnections. The connection works. Not investigated: look only if
  reconnections become unstable or the warning appears outside them.

### USB network that fails under load (open)

- **Symptom**: after 2.7 GB at 27 MB/s, the USB network delivers packets of zeros (`Wrong NTH
  SIGN`), programs die, the PC sees an unknown USB device. At 10 MB/s in 64 MB blocks, more than
  4 GB passed with no error.
- **Cause**: not established. Zeros and dying programs after filling memory are the same
  symptoms as the RAM defect, which was probably the cause.
- **Check first**: limit copies to the phone (`scp -l 80000`) until proven otherwise.

## Screen and touch

### Touchscreen firmware load times out

- **Cause**: DMA on the touchscreen's SPI bus. The load never finishes with DMA on.
- **Fix**: disable DMA on that bus in the device tree.
- **Check first**: the same phone model can ship with different touchscreen controllers (see the
  [hardware notes](ARCHITECTURE.md#hardware-notes)). Read which one you have, then pick the driver.
  Its firmware is the vendor's.

### UI stuck at half the frame rate

- **Symptom**: a WebGL page never goes above 22 to 28 frames per second, whatever you simplify.
- **Cause**: the GPU runs at its lowest clock (377 MHz of 1001), as the bootloader leaves it, and
  the kernel does nothing to raise it.
- **How it was found**: reading the real clocks (`/sys/kernel/debug/clk`) and comparing them to the
  vendor's frequency table, then measuring the frame rate at each frequency: 23, 37, 45, 55. An
  earlier belief, that the way the screen is updated was the limit, was never measured and was wrong.
- **Check first**: on a new phone, compare every important clock (CPUs, GPU, memory, buses) with
  the vendor's maximum as soon as the system boots. The bootloader leaves some at the maximum and
  some at the minimum. Attribute a speed limit to a cause only after measuring it.

### Black screen after an hour and a half

- **Symptom**: the UI disappears for good; the screenshot tool fails with "Bus error"; the browser
  log says `TransferBuffer::Initialize() failed`.
- **Cause**: `/dev/shm` full. `du` showed nothing because the files were deleted but still open.
  Counting open files per process (`ls -l /proc/<pid>/fd | grep /dev/shm`) found the page's process
  with 935 files of 2 MB, one every five seconds: a request whose reply was never read. The
  browser keeps the channel of an unread reply until a memory cleanup that a light page never
  triggers.
- **Fix**: every request goes through one function that reads the reply to the end.
- **Check first**: after adding anything that runs in the background, look at `df /dev/shm` after
  half an hour.

### A JavaScript name defined twice

- **Symptom**: the page keeps drawing when the screen is off (29 frames per second with the
  backlight at zero); in another case, an event that makes the page reload itself forever (the
  screen flashing on and off).
- **Cause**: JavaScript does not warn when two functions or `var` variables have the same name.
  The second hides the first. In one case a `var paused` inside a function hid the page-level
  `paused`. In another, a new event named like an existing one was resent to every connecting page,
  which reloaded, reconnected and received it again.
- **Check first**: search for a name before using it, in the page and in the bridge. An event that
  performs an action must never be among the events replayed to new clients.

### The screen turned off in the middle of something

- **Symptom**: it went off a minute after the last touch, also with the camera open or the agent
  at work; `pim screen on` on a screen already on did not restart the count.
- **Fix**: the count restarts at everything that happens, and the page tells the bridge every five
  seconds whether the keyboard or the camera is open.

### "Send" also opened the pages

- **Symptom**: pressing "Send" closed the keyboard and opened the pages list.
- **Cause**: the keys act when the finger lands. "Send" closes the keyboard, the pages button
  appears under the finger, and the finger lifting there arrives as a tap on it.
- **Fix**: a touch that started on the keyboard counts for nothing else. A tap between two keys
  counts for the nearest, and holding delete repeats.

### A laptop drawn from polygons did not look like a laptop

- **Fix**: render a real 3D model on the PC in 19 poses into one image (`branding/laptop/`); the
  page shows the pose near the entity's rotation. No 3D on the phone.
- **Traps**: headless Chrome saves a white background even for a transparent page unless given
  `--default-background-color=00000000`; material colors must be converted from sRGB or they come
  out washed; with low light the ground shadow stretches to the edge and widens the crop.

### Chat text hard to read on light walls (open)

- The entity's phrase (white) and the person's (small, gray) almost vanish on light walls. They
  need a stronger background or shadow, as in the full-screen mode.

## Audio

### Recording gives zeros without the firmware

- **Symptom**: the audio interface, routing and DMA written exactly as in the vendor's kernel give a
  stream of the right size but all zeros, with any format and volume, while the samples are on the
  wire.
- **Cause**: on this chip the stream format and the DMA are handled by the audio processor's own
  firmware.
- **How it was found**: reading the I2S pins from the kernel with the bit clock slowed down on
  purpose (divider 768 instead of 48) to see the samples on the wire, and writing a marker into the
  buffer before recording to tell "wrote zeros" from "wrote nothing".
- **Fix**: start the original firmware from a module and talk to it with the same messages as
  the vendor's driver. The firmware's own log (kept in its memory, shown in debugfs) tells what
  it dislikes: a wrong message code, a buffer too big.

### What the audio firmware needs from the module

- **Clock frequencies 7% off**: asking a divider for the nominal 73.728 MHz makes the kernel pick
  the divider afterwards, because the PLL is not an exact multiple. Ask for the quotient rounded up
  (`DIV_ROUND_UP(parent rate, divider)`).
- **Unmapped areas**: the firmware touches areas it does not need (memory shared with the modem,
  a register). An access with no mapping stops the sysmmu and the firmware with it, and the error
  stays even after the audio processor is switched off, until the phone reboots (the next boot
  looks as if it failed on the same address, but it is the old error). Everything not mapped points
  to one spare page.
- **Its log is the guide**: it said that a message code was wrong ("Invalid ABOX config MSG: 9":
  this firmware does not want the frequency) and that a buffer was too big ("Not enough memory").
  After a rejected buffer it stops.
- **Too early**: for two seconds after start the firmware looks for the modem
  (`CP Magic Code is not ready`) and ignores requests.
- **Write position**: the counter in WDMA4's status register counts 16-bit samples and wraps at
  65536. With another buffer length the position cannot be derived (Samsung's kernel rebuilds it
  with a round of guesses): the buffer was made 65536 samples long, and the counter is the position.
- **The interrupt controller** of the ABOX is not touched directly: it goes through the secure
  firmware (an `SMC` call), as the Samsung kernel does.
- **Level**: with the analog gain at maximum (+20 dB) the background noise is about 5 out of
  32768, so the microphone is multiplied by 8 in software; the phone's vibration reaches 3500.

### Clocks off, kernel hung

- **Symptom**: unloading the first test audio module stopped the kernel; the watchdog rebooted.
- **Cause**: switching off clocks of the audio block, and reading registers of blocks that are off.
  The bootloader leaves audio, sensors and camera powered and clocked.
- **Fix**: boot with `clk_ignore_unused pd_ignore_unused regulator_ignore_unused`. Removing these
  to save power would hang the kernel at the first read of a block that is off.
- **Also**: read only registers that appear in the vendor source. Reading an offset that does not
  exist inside a module's load function left the module stuck and `reboot` could no longer
  finish; only `echo b > /proc/sysrq-trigger` helped.

### Only the first load after power-on worked

- **Symptom**: reloading the audio module starts the firmware, which reads one message and stops:
  `CP Magic Code is not ready` at `cnt=1`. Its heartbeat timer never starts.
- **Cause**: the audio block is not reset when its core is switched off, and what is left half-done
  fools the new firmware: (1) the timer interrupt line stays high, so the controller, which waits
  for an edge, sees none; (2) an interrupt that was being served stays "in progress" and is never
  delivered again; (3) the core's priority stays raised and blocks the lowest-priority interrupt
  (the heartbeat) until reboot.
- **How it was found**: reading the timer every 200 microseconds right after starting it by hand
  (it counted, then stopped at zero: single-shot, not stuck), then switching its interrupt off and
  seeing that the expiry did not leave it pending, and finally setting it pending by hand: the
  firmware started. A bit that lowers the line, absent from the vendor source, was found by trying
  one write at a time.
- **Fix**: lower all timer lines before releasing the core; clear in-progress and pending
  interrupts at load; at unload, switch the interrupt controller off first, wait 5 ms for the core
  to finish, and only then switch off the core.
- **Wrong turns**: interrupts 68 and 84 "never enabled" (they only turn on with an open stream);
  zeroing the timers; cutting and restoring the internal power; restarting the timers from the
  kernel as Samsung does on resume (they restart, count one millisecond and stop again); raising the
  priority of the two interrupts. Switching off the power domain did not help.
- **Check first**: a block with its own processor and firmware does not reset when you stop the
  core. Before calling it working, unload and reload it ten times. A test that measures the audio
  (`scripts/tests/abox_reload.py`) must also measure the noise floor: a fixed one-second window mistook
  a sound hidden by room noise for silence.

### A plausible but wrong standby mask

- **Symptom**: the unload of the audio module would have timed out every time and left the memory
  mapped.
- **Cause**: the "core stopped" bit mask of the second core was deduced from the first one's
  (`0x11`) instead of read from the source. The real one is `0x01`
  (`samsung,pmu_standby` nodes of the vendor device tree).
- **Check first**: take register values from the vendor source for your model. A plausible and
  wrong value passes every consistency check and only fails on real hardware.

### The speaker, first time

- It worked at the first attempt, because: (1) the amplifier is not on the codec but on an I2C bus of
  its own that the device tree had switched off, the only thing that needed a new boot image;
  (2) the initial values of Samsung's `mixer_paths.xml` already route the first stream (RDMA0) to
  the output, so it is enough to say that the amplifier interface transmits that output, with no
  topology; (3) playback takes the same messages as capture; positions are not sent but read from
  RDMA0's status register (period in bits 27-20, bytes in the period in bits 19-0: here the vendor
  header is right, unlike WDMA4); (4) the stream reads memory in a circle without knowing how far
  it was written: the module zeroes what was played, or the lap before is heard again; (5) the first
  test was 30 dB under Samsung's volume with a tone at a tenth of the amplitude, the amplifier
  switched on after the bit clock and off before it stops; with nobody to listen, the microphone
  verified it (a 750 Hz tone at amplitude 295 on a floor of 2).

### An I2C amplifier that is silent one boot in three

- **Symptom**: the speaker is mute in some boots, and nobody noticed because tests ran after
  "good" boots.
- **Cause**: three, all on the amplifier's I2C bus (internal pull-ups only): writes lost while the
  chip was still resetting; reads returning wrong values without any error while it powers up
  (the driver then modified registers starting from a wrong read); the search for the chip at load
  failing at the first error.
- **Fix**: wait 10 ms after the reset; keep a copy of the registers, compute from the copy, read
  every write back and repeat if it does not match; retry the search five times. The amplifier
  restarted at the module's volume, louder than the one chosen: the module starts at the
  minimum and the bridge applies the chosen volume whenever the speaker appears.
- **Left open**: about one load in thirty came up mute with every register right (amplifier on,
  clock present, stream running). The measurement was not clean: the room was noisy and the owner
  was turning the volume down.
- **Check first**: a chip just reset does not listen at once. Test on many boots, not one. An
  acoustic test measures the noise floor too and shows when the sound is there, not only how loud.

### The two channels of the microphone are not equal (open)

- The two channels of `/dev/abox-mic` were assumed equal; they are not. `mangos-mic` uses the left.
  Maybe there is a second microphone.

### Pages and the speaker: no sound card, one player at a time

- **Symptom**: a game the agent made has no sound, and the agent notices and tries to work around
  it. Two things playing at once cannot: `/dev/abox-speaker` opens once.
- **Cause**: the phone had no ALSA card (`/dev/snd` absent), so the browser had nowhere to send
  audio. The speaker is a character device of ours, not ALSA.
- **Fix**: the kernel's ALSA modules (`soundcore`, `snd`, `snd-timer`, `snd-pcm` and the virtual card
  `snd-aloop`, enabled in the configuration but never built) installed with `install_modules.sh`;
  `/etc/asound.conf` makes `default` a `dmix` mixer on the virtual card; the `mangos-sound` service
  reads the mix from the other side and writes it to the speaker, which it opens only while there
  is sound.
- **Second trap**: even with the card, a page's Web Audio context stayed `suspended`: Chromium
  does not let a page play without a tap. The pages here are made by the agent on request, so
  `--autoplay-policy=no-user-gesture-required`.
- **Check first**: `/dev/snd` and `arecord -l` list the loopback card. The delay added by the chain
  (mixer, virtual card, `arecord`, the speaker's ring) was estimated under 150 ms, not measured.

## Cameras

### The sensor answers, but nothing arrives in memory

- **Symptom**: the sensor returns its ID, the receiver reports overflow and no frame reaches
  memory.
- **Cause**: four things that were not in the source. The block clock was 50 MHz instead of
  400 (the receiver has to absorb 577 million pixels per second). The receiver's single-channel mode
  drops lines; the per-virtual-channel mode is needed. The packed format uses 12 bits per pixel,
  not 10 (6128 bytes a row instead of 5104). The first frame has lane-lock errors; the third is the
  good one.
- **How it was found**: filling the buffer with a known value, using the sensor's color bars as a
  known image, and trying the variants one at a time. The packed-format error wrote outside the
  buffer once, into the reserved zone the system uses while the camera is idle: the phone was
  rebooted at once and the file system was clean.
- **Check first**: write the capture specification from the vendor source before coding. The first
  capture with a new format goes with a guard as large as all the space left. A sensor's clock may
  be missing from the kernel's clock driver; the bootloader often leaves it routed already. The
  kernel's own sensor driver did not start because it wants exactly 24 MHz and the board gives 26:
  our module reuses the register tables of the kernel driver (`scripts/camera_tables.py`) and
  corrects the clock-dependent registers. The frame buffer is uncached device memory: marking it
  "not written" in the whole buffer cost seconds per shot, in the first 64 bytes of each row it
  costs nothing.

### Bringing up a camera: what cannot be guessed

- The sensor's I2C address is `0x2d` (Samsung writes `0x5A`, 8-bit); its supplies come from a
  dedicated chip on another bus; its clock is missing from the kernel clock driver, which skips
  those registers, but the bootloader leaves the mux, divider and gate right and one bit outputs
  the clock; the camera block is left powered with every clock gate open. Reading power and clock
  state first cannot hang the kernel; reading a block that is off would.
- The front camera's receiver is called the "secure channel" by Samsung, and it was not known
  whether the normal kernel may read it. It was tried in steps from a throwaway module with the
  watchdog on: sensor supply and ID, then the clock gates, then one read, the receiver's version,
  announced in the log first. It reads fine: "secure" only means the firmware may take it away
  if asked.

### From the test module to the driver

- **Three shots per photo**: reading the device from the start made the shot, but a light meter
  reads in pieces, each time from the start. A shot is requested by a written word, and
  reading only returns the last frame.
- **numpy on every core**: `OPENBLAS_NUM_THREADS=1` before importing it (see "Seven CPUs busy").
  The brightness curve is applied from a table, not point by point.
- **The sensor does not confirm a write** once in fifteen shots (error -6): writes are repeated up
  to three times.
- **The focus motor** (DW9808 on `hsi2c_0`, address `0x0c`, registers of the DW9807 the kernel
  knows) was off in the device tree. Web search did not find it; it was in the list of Samsung
  source files already downloaded, to be read first.

### One camera at a time

- **Cause**: the two cameras share the memory channel (DMA0), the output selector, the D-PHY and
  the processing block. A single open flag covers the module and a second concurrent open gives
  `EBUSY`. It is not about memory: the two frames (31,516,160 bytes) fit in the 32 MiB reserved;
  an early explanation blamed memory, with one flag per device, and was wrong.
- **Check first**: before writing the second driver, list what the first one uses that is shared.

### Live preview, focus and orientation

- **No low-resolution mode needed**: the sensor stays on at full resolution, the output rewrites the
  same buffer thirty times a second and the reader takes a square of every eighth point (30 ms). An
  image is sometimes between two frames; for looking, it does not matter. The real shot, from on,
  waits for two frames and switches off.
- **Focus is searched on the preview**: fourteen positions in two seconds, measuring the sharpness
  of the center. At rest the lens is not in focus on anything near, so light and focus are always
  adjusted.
- **Orientation**: the sensors are mounted sideways. The rear needs 90 degrees clockwise (checked on
  a scene); the front 270 with mirroring in the preview, as phones do (not checked on a scene).

### Front photos looked almost grey

- **Cause**: the colors were there but washed out (mean difference between the highest and the
  lowest channel 9.5 out of 255). With no image processor the development did black level, white
  balance and a curve; it lacked the color correction matrix, needed because the sensor's filters
  overlap.
- **Fix**: a matrix after white balance, in linear light and integer arithmetic: 9.5 became 21. It
  is the typical matrix of small sensors, not measured on this one.
- **Open**: measure it by photographing known color samples, and look at the rear camera, which goes
  through the same development with no matrix.
- **Not the cause**: a wrong color-filter layout (the four sites have red-like, two equal green and
  blue-like averages) or a wrong sensor black level (the darkest raw point is 64, as declared).

## Sensors

### Accelerometer and compass are behind a microcontroller

- **Symptom**: no accelerometer or compass anywhere in the device tree.
- **Cause**: they are wired to a microcontroller inside the chip (CHUB) with its own firmware, not
  to the CPU.
- **Check first**: before declaring sensors lost, look at whether they sit behind a sensor hub, and
  read the power-management registers: they tell at once if it is powered. The kernel copies the
  firmware into the hub's RAM, releases it from reset through the PMU and talks to it through
  queues in that RAM and a mailbox. The compass came by extending the same module and was tried
  first from the debug commands of the running module, without rebuilding.

### The compass reads about 400 microtesla (open)

- **Symptom**: about 390 µT with accuracy 0, where the Earth's field is about 50. Once it read about
  16, with no note of the conditions.
- **Known**: the same value comes out with the previous module version and after a reboot, so it is
  not the module's corrections.
- **To understand**: the position of the phone relative to magnetic objects, its own magnetization
  (speaker, vibration motor), the calibration (the firmware computes it; collecting and keeping it is
  not done) and the matrix (the vendor device tree has two, with and without the NFC antenna; the
  one without is used).
- **Check first**: write down the conditions of every reading.

### Motion sensors that wake the phone for nobody

- **Symptom**: with the sensors on at a fixed period, about twelve interrupts a second at rest for
  a reader that comes rarely.
- **Fix**: the bridge samples slowly and speeds up only while a page reads them.

## Power, frequencies and temperature

### CPU frequency stuck

- **Symptom**: no `cpufreq` directory; `cpufreq-dt` in `devices_deferred`; both clusters at 1053 MHz
  (the chip goes to 1846).
- **Cause**: on this chip frequency and voltage are changed by the power-management firmware (ACPM),
  by request. The clock driver lacked the Exynos 850 table, and the right channel is 0.
- **Wrong turn**: the vendor device tree says channel 5 for `acpm_dvfs`. Channel 5 is interrupt-driven
  and the driver refuses it; forcing it fails. The clock-controller node of the same tree says 0.
  A test kernel built to investigate channel 5 is the one that did not boot (see above).
- **How it was found**: searching the kernel lists for the chip name together with the subsystem
  (someone had posted the same support for another board with the same SoC), then confirming on the
  running phone with a module that sends one request.
- **Particularities**: asked "what frequency are you at", the firmware always answers with all bits
  set, so the driver remembers the last requested frequency, and the first one has to be requested
  explicitly.
- **Check first**: search the lists for chip plus subsystem before writing code. When the vendor
  tree gives two answers, try both from a module, read-only.

### The chip temperature cannot be read

- **Symptom**: `/sys/class/thermal` has only the battery.
- **Cause**: no driver. The three sensors can be read through the same firmware, channel 9.
- **Fix**: our `acpm_thermal` module registers three thermal zones with the vendor's thresholds
  (throttle from 76 degrees, power off at 115) and hooks the CPU frequency to them.
- **Note**: the 115 degree shutdown really powers off only with no cable; with the cable the phone
  reboots and climbs again with the chip still hot.

### The charger's bus does not answer

- **Symptom**: every read on `i2c_0` times out.
- **Cause**: the bus has no external pull-up resistors.
- **Fix**: enable the SoC's internal pull-ups in the device tree.

### Seven CPUs busy all night

- **Symptom**: with the voice service on and nobody talking, load stays at 10 and the phone at
  55 degrees.
- **Cause**: `numpy` brings OpenBLAS, which starts busy-waiting threads. Switching off the wait in
  `onnxruntime`, the other suspect, changed nothing.
- **How it was found**: `top -H` showed seven threads always running, the first created.
- **Fix**: `OPENBLAS_NUM_THREADS=1` before importing `numpy`.
- **Check first**: after adding any service, `uptime` at rest and wakeups per second
  (`/proc/interrupts`). Polling every 5 ms instead of waiting for an interrupt was another
  400 wakeups a second: the interrupts were in the vendor device tree, and the modules ask
  for them (`irq_create_of_mapping`, with no node of our own). The ABOX interrupt is level
  triggered and stays high until the controller is told what is pending, so the handler
  acknowledges first, and a module that fires a hundred times in a row for nothing switches it off
  and polls again.

## Voice and models

### The model chosen on a synthetic voice fails on a real one

- **Symptom**: phrases like "La hai buisca a portare una foto della mia faccia" arrive to the agent.
- **Cause**: the speech model (`base`) was chosen on a synthetic voice, on which it made one error.
  On a real recording of 61 words: `tiny` 20 wrong, `base` 20 to 24, `small` 8, Parakeet 2.
- **Not the cause**: the microphone (the voice is 21 dB above the noise at 30 cm).
- **Check first**: evaluate speech models on a real person reading known sentences. Measure the word
  errors. One recording is not enough to tune on: two words flipped between runs when a cut moved
  by a tenth of a second, and adjusting more on it would fit the recording, not the voice.

### Same model, four times faster

- **Cause**: the slowness was not the model, it was how `whisper.cpp` runs it. The same model in
  8-bit ONNX, run by `onnxruntime` with the `onnx-asr` library, transcribes in half the audio
  duration, against twice.
- **How it was found**: web research. Other processors reported times thirty times under the audio
  duration for that format, while ours took twice the duration.
- **Check first**: when your numbers are far from other people's with the same model, the runtime
  is a suspect. Profile node by node on the phone. This Cortex-A55 has no 8-bit dot-product
  instruction, so 8-bit matrix products are slow (65% of the time); the 24 depthwise convolutions
  are far better in floating point (`scripts/parakeet_mixed.py`), 16% faster. Whole-model floating
  point was twice as slow, and with signed 8-bit weights the other convolutions slow down three
  times: measure each variant, also the one you believe slower.
- **Dead ends**: re-transcribing the tail at every breath (the model is busy exactly when the
  phrase ends: the wait grew to 7.7 s); a statically quantized variant (25% faster on short phrases,
  but it stops after twenty seconds); an Italian-only model (twice as fast, 5 words wrong in 61,
  and it does not know foreign words); the CPUs held at maximum (no difference); silence added
  around a short segment (worse).
- **What stayed**: each second of speech is transcribed once, at the first 300 ms pause, and that
  text is final; the wait after the last word is about 2 s, of which 0.8 s is the silence needed to
  know the phrase ended. Loading takes twenty seconds and 840 MB, so the model lives in a service.
  Going from four threads to eight gains only 40%.

### The transcription repeats the same phrase dozens of times

- **Symptom**: "Mi senti Pima. Mi senti Pima. ..." forty times (with Whisper).
- **Cause**: a known Whisper defect: with silence or noise at the end it loops on the last chunk
  until it runs out of room. The retry with more randomness that normally stops it was turned off
  because in another case it ran for minutes.
- **Lesson on method**: a user had reported it at the start of the day. We attributed it to what
  we were doing at that moment without reading what had reached the agent. What reaches the agent
  is in its transcript (`/home/pim/.claude/projects/*/*.jsonl`): verify a report there before
  blaming anything.

### Whisper's early problems, still worth knowing

- **Where the time went**: not in loading the model but in starting the program for every phrase.
  The same model kept in memory by a server was up to eight times faster than the command. Another
  allocator (`mimalloc`) crashed Python. Importing `onnxruntime` in the command alone cost seven
  seconds: only the service imports it.
- **A model that invents for minutes** on a phrase cut in the middle (211 seconds for two of audio):
  its "temperature fallback" retries. Every transcription gets a time limit.
- **Previews**: re-transcribing the phrase from the start each time slows down the longer one speaks.
  Fix the phrase in pieces at pauses and re-transcribe only the tail.

### A sentence cut in half

- **Symptom**: at a lower volume the phrase was closed after two seconds.
- **Cause**: the background-noise estimate rose 2% every 20 ms even while someone spoke, and in a
  second it reached the voice, which then counted as silence. The minimum threshold (250) had been
  chosen before any measurement.
- **Fix**: the estimate rises only when nobody speaks, and much more slowly; the minimum went down
  to 100.

### The model writes "Yeah." for silence

- Four seconds of an empty room gave "Yeah.", which reached the agent as a phrase. A reply made only
  of an interjection is discarded.

### Names and cloud transcription

- Without key terms, "Ciao PIM, mi senti?" arrived as "Ciao, mi senti?". The names of the entity and of
  the system are given to the cloud model as key terms.

### The synthetic voice: where the time and the echo went

- **Thirteen seconds before every sentence**: `onnxruntime` takes 13 seconds to prepare the model,
  the same for a 28 MB and a 63 MB one, with optimizations on or off: one core full of tiny memory
  requests (5,600 `mmap` in `strace`). The model stays loaded in a service, and the command hands
  it the text.
- **The entity hears itself**: if the microphone listens while the speaker is saying a reply,
  the reply is transcribed and returns as if the person had said it. Listening first silences the
  voice; what is already in the speaker, at most a second and a half, finishes before the
  microphone opens.
- **Microphone busy after the phrase**: the input stayed open for the whole transcription. It
  closes as soon as the phrase is cut.

### The voice started only when the reply was complete

- **Fix**: Claude Code runs with `--include-partial-messages`; the text arrives in pieces, the agent
  cuts it into phrases and each finished phrase goes at once to the screen and to the voice
  service, over one connection that keeps the speaker open. What the model writes before using a
  tool is spoken too. The first sound comes about 0.7 s after the first phrase.
- **Related, found later**: the flow stopped working when the accumulated text passed a cap of 280
  characters, and the accumulation included phrases already said between tools: in a job with
  several steps the entity announced, worked and in the end said nothing, with the text only on the
  screen. Every finished phrase is spoken whole; the cap applies to the line shown.

### Two spoken phrases in a row, and the reply to the wrong one

- **Symptom**: the person reopens the conversation and speaks while the entity is still working on
  the previous phrase: the reply to the first reopened the microphone while the entity started the
  second reply; its voice was cut, or the microphone heard it and delivered it as the person's.
- **Cause**: the message from the agent saying "I finished replying" did not say to which phrase it
  referred; the bridge applied it to the one it was waiting for at that moment.
- **Fix**: the bridge numbers each spoken phrase, the agent returns the number, and the bridge
  ignores messages for another one.

### The cloud voice arrives quiet and uneven

- **Symptom**: the level of Deepgram's voice differs from sentence to sentence (a mean of -25 to -27
  dBFS over three sentences), and some replies sound lower.
- **Fix**: a fixed gain first; then a leveller that brings every sentence to a target level while
  it arrives, counting only the pieces with speech, starting from the gain of the previous sentence
  (at most x4) and changing it gradually; peaks above 75% of full scale are rounded off instead of
  clipped. The cloud voice has no speed control in Italian: `sox tempo` speeds it up keeping the
  pitch.
- **Not proven**: whether an open microphone lowers the speaker. In the audio driver it does not
  touch the amplifier.

### A cloud failure is reported, not hidden

- A cloud voice or transcription that does not answer is not silently replaced by the local one:
  the sentence is not said or transcribed, and the reason is logged and told. The person hears a
  different phrase for a missing or refused key, no network and a microphone that gives no audio. A
  service whose configuration cannot work stops after five restarts in a minute instead of
  restarting for ever.

## The agent

### Two Claude Code sessions that talk to each other

- **What works**: independent `claude -p` sessions, each with a name (`--name`), that write to each
  other with `SendMessage`. The receiver needs `{"crossSessionInbound": "accept"}` in its
  `--settings`, or in `-p` the message waits five minutes and is lost. A round trip takes about
  five seconds with no user message and no code of ours in between.
- **What does not**: teams (the tool to create them does not appear in `claude -p`), and
  subagents, which live inside the session of the one that started them and follow its restarts and
  compactions.
- **Not what you would expect**: Haiku is not faster than Sonnet at answering a short sentence
  (about 1.2 s to the first word in both): the time is Claude Code's and the network's. Two models
  are worth it because the conversation goes on while the heavy one works. A session costs 4-5 s to
  start and about 95 MB at rest: keep sessions open.
- **Check first**: `SendMessage` reaches a session by its name: no `ListAgents` is needed. Without
  `--name` a session is named after its folder plus a hex suffix. A lookup is also one more model
  call, during which the person hears nothing.

### A resumed conversation ignores new instructions

- **Symptom**: after changing the instructions the entity, resumed from its old conversation,
  keeps behaving as before. The talker kept building pages itself, two trials out of two; in a
  new session, with the same instructions, it delegated at once.
- **Cause**: Claude Code records the system prompt at the first request of a conversation and
  reuses it in every resumption until the conversation is compacted.
- **Fix**: `--system-prompt-snapshot off`, so the instructions as they are now apply also to
  conversations that are resumed.
- **Check first**: if an instruction change has no effect, compare with a new session.

### A turn that ends with a tool and no text

- **Symptom**: after `SendMessage` the talker said "I passed the work on", which the person must not
  hear: to them there is one entity.
- **Cause**: in `claude -p`, a turn that ends with a tool call and no text receives from the
  program the message "Your previous response had no visible output..." and the model writes a
  sentence anyway.
- **Fix**: the instructions plan the turn: first a sentence, then the tool, then a few closing words,
  all in the same turn.
- **Check first**: read the transcript of a turn that ends with a tool.

### Errors the person can act on

- A turn that fails is told with a phrase that fits: a missing or invalid token, the subscription's
  limit reached, no network. The first pattern found in the error text decides; otherwise a generic
  phrase. A session resumed that closes within seconds, without an answer, had no conversation to
  resume: the marker is dropped and the next start is a new conversation.

### The entity put an old page back

- **Symptom**: the microphone, the volume bar and the camera window vanished from the screen, and
  `/dev/shm` filled up again.
- **Cause**: asked for four rooms in a row, the agent wrote the corridor starting from a copy of the
  page it had kept since the night before, and installed it as the UI: everything added the same
  day, fixes included, was gone.
- **How it was found**: the page's checksum on the phone differed from the repo's; `find -newer`
  on the system files showed three changed after the last commit, and the working folder had the
  starting copy and the new one.
- **Fix**: a three-way merge (`git merge-file`) of the repo's page, the agent's starting copy and
  its page. The installer keeps the checksums of what it installs
  (`/var/lib/mangos/installed.md5`) and stops if a file on the phone changed, so the agent's work is
  not covered in silence. The agent's instructions say to start from the file installed at that
  moment, to keep the previous one beside it and to look at the screen afterwards.
- **Check first**: `cd / && md5sum -c /var/lib/mangos/installed.md5` on the phone, then
  `ls -lt /home/pim`.

### "It does not answer"

- **Symptom**: three messages with no answer, then three answers.
- **Cause**: nothing broke. The agent handles one message at a time and was in a long turn. The
  screen said nothing about it.
- **Fix**: show the queue ("in queue, read as soon as it finishes") and show that the agent is
  working (the laptop appears two seconds after the first tool of a turn).
- **Check first**: the timeline in the agent's transcript is the first place to look.

### "Look at this" took 14 to 24 seconds

- **Cause**: six wastes, none of them "the model is slow":
  - the conversation was 177,000 tokens, carried by every call. The summary after a pause never
    fired: the pause counter lived in memory and restarted with the agent, and the measure was wrong
    (it summed the calls of a turn). It measures what the model reads at the last call and
    summarizes right after a reply above a threshold. A photo it looks at costs thousands of
    tokens;
  - Claude Code started at the first message after every restart (3-4 s): it starts with the agent;
  - three model calls where two were enough (take the photo, then ask to open it): the instructions
    say to call `pim photo <file>` and the read of that file in the same message;
  - photo development was 1.6 s of floating-point arithmetic on three million points: a table per
    color, a tenth of a second;
  - program start loaded numpy and Pillow, 1.1 s: a service keeps them loaded;
  - the preview waited three fixed seconds plus a fourteen-position focus search: light and focus
    restart from the values of the last shot, focus is searched around them (five positions), and
    the shot is taken as soon as they are right (a second and a half at least). With little light
    gain is preferred to a long exposure, which slowed the preview.
- **Result**: 7-8 seconds.
- **Check first**: how many tokens the agent reads at each call and how many calls a request makes.

### The same phrase to start every job

- The phrase before a long job is written by the model, not by the system. The instructions gave two
  examples and Haiku copied the first, every time. Ten examples and the rule "never the same twice
  in a row" gave three different phrases in three jobs.

## Tools and method

### Kernel sources that are not the running kernel's

- After a test, the source tree may hold something else. Before compiling a module, compare
  `include/config/kernel.release` with `uname -r` and check `git status`. A module built from
  different sources may load and hang the phone.

### A driver that is missing does not always need a new kernel

- If its node is already in the device tree in use, compile it as a module and load it on the
  running phone. Vibration came that way: the node was there from the A13 tree, only the
  configuration entry was missing. On a new phone, list at once the devices of the device tree
  with no driver (`/sys/bus/platform/devices/*/driver` absent).

### Scripts sent over SSH

- A list with one item per line in a remote command arrives split into several commands: pass lists
  on one line or in a file.
- An apostrophe in a message inside `${1:?message}` nested in `$(...)` breaks the script.
- A script fed on standard input (`ssh host sh -s < file`, or to `bash`) loses lines when a command
  inside reads standard input: `ssh`, or `pim`, which uses `curl`, swallows the rest of the script,
  without an error. Copy the file and run it, or give those commands `< /dev/null`. From Windows,
  `bash <(tr -d '\r' < file)`.
- A script that opens many SSH connections in a row sometimes loses one ("Connection closed"), and
  that step is skipped without an error: a reboot "done" never started. Copy files in one archive;
  after a reboot, check that `uptime` restarted from zero.
- Copies to the phone at full speed corrupted its USB network once: limit with `scp -l 80000`.

### A test that reads "the first line" of a line protocol

- **Symptom**: the stream test of the voice service reported a wrong result.
- **Cause**: the service had begun to write a line per sentence heard before the result code, and
  the test took the first line as the result.
- **Check first**: when a protocol gets new lines, grep the tests that read it. A test reads up to
  the line it wants, not the first one.

### A change team interrupted halfway

- After an interrupted batch of agents, part of the changes was made and part not, and the
  reports did not say which. Do not trust the reports: look at the state (`git status`, `git grep`,
  `ls` for every name cited), and run every changed script once, with its failure paths.

### Checksums are computed on the real files

- Pinned versions and checksums of downloads are worth something only if computed on the real
  files. One written from memory, or copied from a report, makes the good file be refused or
  protects nothing. The image flasher computes its checksum on the PC, compares it on the phone after
  the copy and again after the write (reading the partition back).

### `pkill -f` kills your own session

- If the text you search for is in your own command, `pkill -f` and `killall sh` also close the shell
  that runs them. Use `pgrep -f` inside a script fed over standard input, then `kill` the number.

### Line endings and the wrong `git`

- Files written from Windows can have CRLF and not run on the phone or in WSL; the `git` of WSL
  saw as modified files that differed only by line ending, and `git add -A` staged sixteen of them
  (ten thousand lines). `.gitattributes` is `* text=auto eol=lf`. Add files by name, and check a
  new file that reaches the phone (`git ls-files --eol`).

### `es2gears_wayland` is not a graphics test

- It dies by itself inside `cage`, with or without the GPU. Use `weston-simple-egl` (package
  `weston-clients`).

### Tests that need a person

- A recording started twelve seconds after the text was shown, without waiting for the person who
  had to speak, held only the voices of other people in the room, and was thrown away. A microphone
  test starts when whoever has to speak says so. A phone in a room with people: ask before opening
  the microphone or playing sound.

### First order of checks on a new phone

1. The way back from a bad kernel, before the first modified kernel.
2. The memory map: `ramwatch` on almost all RAM.
3. The disk: write random data with memory full and read it back from the disk directly, several
   times (`scripts/tests/full_memory_writes.sh`); the file system check at boot.
4. Only then: network, packages, graphics.
5. Random crashes in unrelated programs: memory and disk first
   (`scripts/tests/file_rereads.sh`), then the programs.
6. Clocks and temperature against the vendor's maximum.
7. A block with its own firmware: unload and reload ten times.
8. An I2C chip: wait after reset, read back every write, test on many boots.
9. Before a new driver: the specification from the vendor source, with file and line.
10. For every service: `uptime` at rest and wakeups per second.
11. For the agent: how many tokens it reads at each call and how many calls a request makes.
12. After every installation: the reboot worked, the services are started, and the screen has been
    photographed in every state it can be in.
