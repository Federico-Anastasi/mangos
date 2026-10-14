# Development

The work cycle on this repository: connect, build, install, verify, and the rules that keep the
phone alive. For how the system is built see [ARCHITECTURE.md](ARCHITECTURE.md); to rebuild from an
empty phone or port to another one see [PORTING.md](PORTING.md).

1. [Connect to the phone](#connect-to-the-phone)
2. [Scripts](#scripts)
3. [Try a driver](#try-a-driver)
4. [Try a kernel](#try-a-kernel)
5. [Verify after an installation](#verify-after-an-installation)
6. [If something goes wrong](#if-something-goes-wrong)
7. [Safety rules, with the reasons](#safety-rules-with-the-reasons)
8. [Style](#style)
9. [Documents](#documents)
10. [Changing the language](#changing-the-language)

Everything runs from WSL (Ubuntu), from the repo folder. Each script starts with a header that says
what it does and how to call it: read it first. Versions and checksums of everything downloaded are
pinned in `scripts/common.sh`.

## Connect to the phone

```
ssh m13 "command"          # from WSL
```

`m13` is an alias in the WSL `~/.ssh/config`, written by `scripts/prepare_env.sh`. It uses
`scripts/phone_connect.sh`, which tries the cable (`172.16.42.1`) and then the Wi-Fi. If the last
known Wi-Fi address does not answer it scans your network's /24 on port 22 for the machine that
presents the phone's key (`M13_NO_CABLE=1` skips the cable, to test the Wi-Fi path).

| To | Use |
|---|---|
| Choose the Wi-Fi network | `bash scripts/phone_wifi.sh ["network name"]` (asks the password in your terminal) |
| Store the Claude subscription token | `bash scripts/phone_token.sh` (paste the output of `claude setup-token`); it restarts the agent |
| Store the Deepgram key, for cloud speech | `bash scripts/phone_deepgram.sh`; it restarts `mangos-speak` |
| See the real screen | `bash scripts/phone_look.sh x.png`, then read the PNG. Also prints processes, memory and temperature |
| Write to the agent | `ssh m13 "pim message 'text'"` |
| Wake the screen | `ssh m13 "pim screen on"` (it turns off after two minutes without touch) |
| Measure | `ssh m13 pim status`: battery, Wi-Fi, temperatures, frequencies, load, free memory, disk, frames per second |
| Choose local or cloud speech, one or two agents | `ssh m13 "pim speech [local\|cloud]"`, `ssh m13 "pim agents [1\|2]"` |
| Give real touches to the UI | See below |
| Run commands with no system booted | `scripts/phone_sh.sh` (the ramdisk's rescue shell on the USB network) |

**Real touches.** Touches simulated by the kernel do not reach the page; the browser's debug port
does. On the phone, `touch /etc/mangos/web-test` and `rc-service mangos-web restart`; copy
`scripts/tests/ui_touch.py` there and run it (its header lists the commands). Remove the file and
restart `mangos-web` when done: while it exists anything on the phone controls the browser. It
does not go through the real touchscreen, so it says nothing about lost or imprecise touches of the
panel, and tapping the microphone opens it for real.

A reboot takes about 35 seconds before SSH answers. After one, check that `uptime` restarted.

## Scripts

| Script | What it does |
|---|---|
| `common.sh` | Pinned versions and checksums, required packages and firmware, shared functions. The others load it |
| `prepare_env.sh` | Prepares the PC: packages, pinned kernel and uniLoader sources, the reference A13 image, the Alpine base, the ramdisk programs, `heimdall`, the `m13` alias. Safe to re-run; lists the Samsung firmware still missing |
| `build_initramfs.sh` | Builds the boot ramdisk |
| `make_bootimg.py` | Assembles the Android boot image around uniLoader |
| `build_m13_boot.sh` | Builds the ramdisk, kernel and device tree and the boot image |
| `build_up_param.py` | Builds the `up_param` partition with the boot screens (`branding/splash.svg`) |
| `flash_boot.sh` | Writes an image into `boot` or `recovery` from the running phone, checking its SHA-256 before and after; `boot` only for an image already tried from `recovery` |
| `push_image.py` | Writes a raw image to a partition over the USB network, block by block, reading each back; the phone stopped in the rescue shell |
| `phone_sh.sh` | Runs commands in the ramdisk's rescue shell |
| `install_mangos.sh` | Installs the system, bridge, agent and UI on the running phone: copies the files that differ, installs packages, Claude Code, the `pim` user, the Piper voice and the Parakeet model if missing, and restarts the services that changed |
| `install_hardware.sh` | Installs the audio and sensor firmware and the modules mangOS loads: `abox`, `chub`, the sound loopback and the camera regulator |
| `install_modules.sh` | Compiles kernel modules for the running kernel (or the next one) and installs them on the running phone |
| `build_mangos_rootfs.sh` | Builds the whole system as an archive, as root |
| `phone_connect.sh` | Finds the phone, by cable or Wi-Fi: it is behind the `m13` alias |
| `phone_wifi.sh`, `phone_token.sh`, `phone_deepgram.sh` | Put the Wi-Fi network, the subscription token and the Deepgram key on the phone |
| `phone_look.sh` | Photographs the real screen from the PC |
| `camera_tables.py` | Extracts the camera sensors' register tables from the kernel drivers |
| `parakeet_mixed.py` | Rebuilds the Parakeet encoder for this CPU (PC, 6 GB of memory) |
| `tests/ui_touch.py` | On the phone: real touches to the UI and reads the page |
| `tests/transcribe_paced.py`, `tests/transcribe_compare.py` | On the phone: feed `mangos-transcribe` a recording at speech pace; compare transcription models on known phrases. No microphone |
| `tests/speak_stream.py` | On the phone: tests streamed speech without playing it |
| `tests/abox_reload.py` | On the phone: reloads the audio module many times and listens for a tone (it plays: warn first) |
| `tests/full_memory_writes.sh`, `tests/file_rereads.sh` | On the phone: disk writes with memory full; the same files read several times |
| `tests/claude_sessions.py` | On the phone: two headless Claude Code sessions messaging each other |
| `tests/mangos_js.html`, `tests/page_sound.html` | Test pages for `mangos.js` and for sound from pages: `pim show <file> full` |

What the headers do not say:

- `install_mangos.sh` is the most frequent step. The agent can edit on the phone the files it
  installs, so it stops if one changed since the last installation (`md5sum -c
  /var/lib/mangos/installed.md5` on the phone says which): bring those edits into the repo first,
  or use `--overwrite`. Files removed from the repo are not removed from the phone.
- `install_modules.sh` needs the sources in `~/m13/linux-wifi` to be those of the running kernel;
  it compares versions and stops otherwise. It works only for devices whose node is already in the
  running device tree.
- `build_m13_boot.sh` builds the ramdisk itself, so it is always current. After boot,
  `/var/log/initramfs.log` must have the `e2fsck` line.
- `build_mangos_rootfs.sh` needs the kernel already built, because it compiles our modules for
  that version. It has not been run end to end on an empty phone.

## Try a driver

A driver whose device node is already in the running device tree is tried as a module, without
touching the boot:

```
bash scripts/install_modules.sh mangos/tools/<module>
ssh m13 modprobe <module>
```

`mangos-watchdog` must be running: if the kernel hangs, the phone reboots by itself within a minute.
For a risky first read of a register, use a throwaway module (`mangos/tools/acpm_test` is the
model) and announce the read in the log first. A module goes into
`mangos/rootfs/etc/modules-load.d/mangos.conf` only after it worked; one that needs Samsung
firmware is loaded by `30-hardware.start` instead, which skips a module that hung the previous boot.

## Try a kernel

1. `bash scripts/build_m13_boot.sh ~/m13/m13-boot/<name>.img`
2. `bash scripts/install_modules.sh --next <modules>`: our modules, built for the new kernel
   before it runs (each image has its own `/lib/modules/<version>`).
3. `bash scripts/flash_boot.sh <image> recovery`
4. `ssh m13 mangos-reboot recovery`; check with `uname -r`. `ssh m13 mangos-reboot` returns to `boot`.
5. Only when the image has worked from `recovery`: `bash scripts/flash_boot.sh <image> boot`, which
   checks it and refuses otherwise unless `--force`.

If a test kernel hangs, the phone can only be recovered from download mode (below).

## Verify after an installation

- The reboot worked (`uptime` restarted) and `ssh m13 rc-status` lists the services as started.
- `ssh m13 pim status` answers with plausible numbers.
- The screen, photographed in every state it can be in (`scripts/phone_look.sh`). A new screen is
  not finished until it has been seen.
- After a boot image: `/var/log/initramfs.log` has the `e2fsck` line; after half an hour, `uptime`
  and `df /dev/shm` (two serious defects showed only with time).
- At rest, `uptime` and wakeups per second (`/proc/interrupts`) for every new service.
- Stability tests run with memory full: `scripts/tests/full_memory_writes.sh`,
  `scripts/tests/file_rereads.sh`.

## If something goes wrong

- **SSH does not answer but the USB is there**: the phone can be reached from Windows on the IPv6
  link-local address of the USB network adapter (`Get-NetNeighbor` lists it).
- **Ping answers but SSH does not complete**: the system is hung. Forced reboot: Volume Down and
  the side key for 7 seconds; if it half answers, `echo b > /proc/sysrq-trigger`. The PC cannot do
  it. The same symptom, with the screen off and no Wi-Fi, comes from memory running out: there is
  no swap, and the watchdog keeps being fed while everything else is stuck. On the phone build
  with at most two jobs and watch `free -m`.
- **The Wi-Fi is gone and the cable is out**: plug the cable back in.
- **postmarketOS (Nura) starts instead of mangOS**: it is the ramdisk's safety net, used because
  the previous mangOS boot did not finish (`.boot-pending`). Reboot.
- **The UI lost a part**: the agent may have edited it. See `install_mangos.sh` above.
- **The phone does not boot after writing a kernel**: download mode, with the phone off: hold
  Volume Up and Volume Down while plugging in the cable, then press Volume Up once. From a phone
  that reboots by itself: cable in, Volume Down and the side key until the screen goes black, then
  without releasing Volume Down move from the side key to Volume Up; at the blue screen press
  Volume Up. Pass the USB port to WSL (`usbipd bind --busid <id>` once, with confirmation as
  administrator; `usbipd attach --wsl --busid <id>` with a WSL window open) and, as root:
  `~/m13/heimdall-grimler/build/bin/heimdall flash --BOOT <image>` (or `--RECOVERY`) with an image
  that works. Use the `heimdall` built by `prepare_env.sh`: the one in the Ubuntu packages reports
  success and writes nothing. One operation per session: after a read or an error, reboot the
  phone.
- **A test kernel hangs in `recovery`**: a forced reboot does not leave it, because the bootloader
  remembers the last mode. Only download mode does.

## Safety rules, with the reasons

Each was learned by breaking something.

- **Partitions.** Write only `boot`, `recovery`, `up_param` and `userdata`. The others hold the
  bootloader and factory data (such as the Wi-Fi address) that cannot be recovered.
  `05-partlabels.start` makes them read-only at boot, because the agent is root; the rule holds
  anyway. The Samsung bootloader and security firmware are signed and cannot be replaced: do not
  propose removing them.
- **Test kernels go in `recovery`**, never straight into `boot`. A bad kernel in `boot` leaves the
  phone unreachable until download mode. Only an image that has already worked from `recovery` is
  written to `boot`. Change one thing at a time, and try first whatever a module can do on a running
  phone. `kexec` does not work on this phone, so it is not a shortcut.
- **New drivers are modules**, loaded by hand on the `boot` kernel with `mangos-watchdog` running: a
  hung kernel then reboots in about a minute. A module enters `modules-load.d/mangos.conf` only after
  it worked, because one that hangs the kernel at boot would hang it at every boot.
- **In a driver that runs at boot, add no memory reads for diagnostics.** Reading a block that is
  powered off hangs the kernel. Try reads from a module on a running phone, and read only registers
  that appear in the vendor source.
- **Values from the vendor source for this exact model**, for bootloader registers and hardware
  registers. Values from a similar model were ignored or wrong here.
- **Copies to the phone are rate-limited**: `scp -l 80000`. At full speed the USB network corrupted
  and the phone needed a forced reboot.
- **Never `pkill -f` or `killall sh` with text that appears in your own command**: it closes your own
  session.
- **Stability tests run with memory full.** The worst defect found (RAM declared larger than the
  real one) did not show with free memory.
- **Memory.** No swap and 3.8 GB: at most two build jobs on the phone, `free -m` while they run;
  a big program is built on the PC.
- **Secrets** (the Claude token, the Deepgram key, the Wi-Fi password) are typed in your terminal
  by the scripts that need them and never go into the repo.
- **Microphone and speaker.** Opening the microphone records the room, and what it hears reaches the
  agent and the network. Ask the owner first, and test on files when you can. A test that plays
  sound is announced, short, and does not change the volume the owner chose. Warn before rebooting
  the phone.
- **Samsung firmware** (touchscreen, Wi-Fi, audio, sensors) stays in `~/m13/firmware` and is never
  added to the repo: it is proprietary.

## Style

- **Comments.** Short, useful to whoever reads the code later, and true as long as the code beside
  them is. Write one only for the why that is not obvious from the code: a constraint, a trap, a
  value that looks wrong and is right. Leave out notes to yourself or to agents, "before / now",
  history, dates, measurements of a test, who decided what, pointers to lines or files, and
  whatever the code already says. A file header says what the file does and how to use it, in a
  few lines. Leave no dead code, unused imports or half-finished stubs.
- **Names** are English and consistent across files: commands and services are `mangos-*`, and UI
  events and endpoints use the same words as the `pim` commands. Before naming an event or a
  function, search for the name in the bridge and in the UI: JavaScript does not warn about two
  functions with the same name. Avoid GLSL reserved words in shaders: a compile error turns off all
  the graphics.
- **The name of the system** is written mangOS, also at the start of a sentence. Identifiers, paths
  and commands stay lower case (`mangos-bridge`, `/etc/mangos`).
- **Line endings** are LF (`.gitattributes`). If a script written on Windows does not start, check
  `git ls-files --eol` for CRLF; a new file that ends up on the phone or in WSL needs the same care.
- **Commands with many quotes** (PowerShell, then WSL, then SSH) break. For file edits use the edit
  tool; for longer scripts write a file and run it. Do not pass a script as `ssh host sh -s < file`
  or to `bash` on standard input: the first command that reads standard input (`ssh`, or `pim`,
  which uses `curl`) swallows the rest of the script. Copy the file and run it, or give those
  commands `< /dev/null`. A list with one item per line in a remote command arrives split: pass it
  on one line or in a file.

## Documents

- **One fact, one place.** Before writing, `git grep` for where it already is. State and what is
  open: `README.md`. How it is built, with the configuration and data: `docs/ARCHITECTURE.md`. How
  to work and the safety rules: this file. Steps to rebuild or port: `docs/PORTING.md`. Every
  problem you understand, with its cause: `docs/LESSONS.md`, the only register: symptom, cause, how
  you found it, what to check first, including the wrong turns.
- **Headers are the usage documentation.** The header of a script or command says how to call it;
  documents do not repeat its options.
- **When a behavior changes**, update the document of its place, and `git grep` the name of the
  service, command or key you changed before you commit.
- **Documents follow the comment rule**: no history, no "before / now", no dates, no notes for
  agents, no names of people.

## Changing the language

The agent and the interface speak Italian. For another language change:

- the UI strings in `mangos/web/index.html`;
- the messages said to the person in `mangos-bridge` (`GREETINGS` and the voice failures) and
  `mangos-agent` (the failure phrases and the worker's start message);
- the language instruction and the example phrases in `mangos/pim/CLAUDE.md` and in
  `mangos/rootfs/usr/share/mangos/pim/` (`voice.md`, `talker.md`);
- the cloud transcription in `mangos-transcribe` (`language=` and the key terms, among them the
  entity's name, which is in the code and not in `/etc/mangos/entity`) and its list of noise
  words;
- the voices: `DEFAULT_VOICE` and `CLOUD_VOICE` in `mangos-speak`, and the Piper voice downloaded by
  `install_mangos.sh`;
- the browser language in `mangos-web`;
- the time zone in `build_mangos_rootfs.sh`.

The local speech recognition model (Parakeet) is multilingual.
