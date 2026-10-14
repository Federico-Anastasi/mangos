# mangOS

mangOS is an operating system for a Samsung Galaxy M13 (`SM-M135F`) with no Android: a mainline
Linux kernel, Alpine Linux on top, and an AI agent, PIM, that lives on the phone and is its
interface. `README.md` says what works; the details are in `docs/` (last section).
`mangos/pim/CLAUDE.md` is different: it is the core instruction file of the agent on the phone.

## Where things are

| Where | What |
|---|---|
| `kernel/` | M13 device tree and our drivers (charger, backlight, touchscreen) |
| `kernel-kconfig/`, `kernel-patches/` | Kernel config entries and fixes to code that is not ours |
| `mangos/initramfs/`, `mangos/rootfs/` | Boot ramdisk; system files over Alpine (services, boot scripts, programs, the agent's role instructions in `usr/share/mangos/pim/`) |
| `mangos/web/`, `mangos/pim/` | The UI (one WebGL page); the agent's core instructions |
| `mangos/tools/` | Our kernel modules (audio, sensors, camera, GPU, thermal, reboot) and test tools |
| `scripts/` | Build, install and connection scripts, and `scripts/tests/`; each starts with a header: read it first |
| `branding/` | Boot screen, and the 3D model the UI's laptop is rendered from |
| `docs/` | The documents listed below |
| WSL `~/m13/` | Working folder outside the repo: kernel sources, firmware, built images |

## Main commands

From WSL (Ubuntu), in the repo folder. The phone is reached with `ssh m13 "command"`.

- Prepare the PC: `bash scripts/prepare_env.sh`
- System, bridge, agent, UI: `bash scripts/install_mangos.sh`, then look: `bash scripts/phone_look.sh x.png`
- Try a driver as a module: `bash scripts/install_modules.sh mangos/tools/<module>`, then `modprobe`
- Boot image: `bash scripts/build_m13_boot.sh ~/m13/m13-boot/<name>.img`
- Write it: `bash scripts/flash_boot.sh <image> recovery`, then `ssh m13 mangos-reboot recovery`

## Safety rules

These break the phone if ignored. The reasons are in `docs/DEVELOPMENT.md`.

- Write only the partitions `boot`, `recovery`, `up_param` and `userdata`. Never touch the others.
- Test kernels go in `recovery`, never straight into `boot`. One change at a time.
- New drivers are modules, loaded by hand with `mangos-watchdog` running. A module enters
  `modules-load.d/mangos.conf` only after it worked.
- Copies to the phone are rate-limited: `scp -l 80000`.
- Never `pkill -f` or `killall sh` with text that appears in your own command.
- Samsung firmware stays in `~/m13/firmware`, never in the repo.
- Ask the owner before opening the microphone or playing sound, and warn before a reboot.

## Language

Code, comments, scripts, commit messages and documents are in English. What the person sees and
hears (UI, agent replies, voice, speech model) is in Italian; `docs/DEVELOPMENT.md` lists what to
change for another language.

## Where to find the details

- `docs/ARCHITECTURE.md`: how the system is built (boot chain, partitions, services, bridge, agent
  and `pim`, kernel modules, configuration and data, hardware notes, security). Read it before
  changing any component.
- `docs/DEVELOPMENT.md`: the work cycle, every script, how to verify, what to do when something
  goes wrong, the safety rules with their reasons, style and comment rules, how to keep the
  documents. Read it before your first change.
- `docs/PORTING.md`: rebuild on the same phone, or port to another one, step by step.
- `docs/LESSONS.md`: problems already found, with causes. Read it before debugging hardware, and
  add every problem you understand.
- `NOTICE`: third-party material and licenses.
