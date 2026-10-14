#!/usr/bin/env python3
"""Reload test for the audio module, to run on the phone (plays a tone: warn first).

Unloads and reloads the abox module many times, and on every round checks that the firmware is
alive (timer 0 counting, all messages read) and that audio really passes: plays two seconds of a
750 Hz tone from the speaker while the microphone records, and looks for the tone in the
recording, a quarter of a second at a time. This also shows whether the sound started late or in
pieces.

Usage: abox_reload.py [--no-reload] [rounds]      (5 rounds). Plays at the current volume.
With --no-reload the module is not reloaded: only the sound test is repeated.
"""
import array
import math
import re
import subprocess
import sys
import threading
import time

STATUS = "/sys/kernel/debug/abox/status"
LOG = "/sys/kernel/debug/abox/log"
TONE_HZ, SAMPLE_RATE = 750, 16000
WINDOW = SAMPLE_RATE // 4
TONE_S = 2
EXPECTED_WINDOWS = TONE_S * 4


def status():
    with open(STATUS, encoding="utf-8") as f:
        return f.read()


def alive():
    """True if timer 0 counts and the firmware has read all the messages."""
    values = set()
    for _ in range(6):
        values.add(re.search(r"timer 0:.*?now (0x[0-9a-f]+)", status()).group(1))
        time.sleep(0.013)
    read, written = re.search(r"queue to firmware: commands read (\d+) written (\d+)", status()).groups()
    return len(values) > 1 and read == written, f"timer 0 {sorted(values)[:3]}, messages {read}/{written}"


def tone_level(samples):
    """Amplitude of the 750 Hz component."""
    sine = sum(c * math.sin(2 * math.pi * TONE_HZ * i / SAMPLE_RATE) for i, c in enumerate(samples))
    cosine = sum(c * math.cos(2 * math.pi * TONE_HZ * i / SAMPLE_RATE) for i, c in enumerate(samples))
    return 2 * math.hypot(sine, cosine) / len(samples)


def by_windows(data):
    samples = array.array("h", data[: len(data) // 2 * 2])
    return [tone_level(samples[i:i + WINDOW]) for i in range(0, len(samples) - WINDOW + 1, WINDOW)]


def play_and_record(seconds=TONE_S):
    """The 750 Hz level in the room before the sound, the level of every quarter second after the
    sound was started, and the module status while it played."""
    tone = array.array("h", (int(8000 * math.sin(2 * math.pi * TONE_HZ * i / SAMPLE_RATE)) for i in range(SAMPLE_RATE * seconds)))
    mic = subprocess.Popen(["mangos-mic"], stdout=subprocess.PIPE)
    silence = mic.stdout.read(SAMPLE_RATE * 2)             # one second; the first half is muted
    speaker = subprocess.Popen(["aplay", "-q", "-D", "default", "-t", "raw", "-f", "S16_LE", "-r", str(SAMPLE_RATE),
                                "-c", "1", "-"], stdin=subprocess.PIPE)

    def play():
        # From a thread: the write waits for the speaker, and meanwhile the mic must be read.
        speaker.stdin.write(tone.tobytes())
        speaker.stdin.close()
    threading.Thread(target=play, daemon=True).start()
    recorded = mic.stdout.read(SAMPLE_RATE * 2)
    midway = status()
    recorded += mic.stdout.read(SAMPLE_RATE * 2 * (seconds + 2))    # three seconds past the tone in all
    mic.stdout.close()
    mic.wait()
    speaker.wait()
    return max(by_windows(silence[SAMPLE_RATE:])), by_windows(recorded), midway


def judge(before, windows):
    """How many windows contain the tone (it must clearly exceed what was already at 750 Hz in
    the room) and a readable summary."""
    threshold = max(100, 3 * before)
    with_tone = [i for i, level in enumerate(windows) if level > threshold]
    drawing = "".join("#" if level > threshold else "." for level in windows)
    return len(with_tone), f"tone {drawing} (room {before:.0f}, max {max(windows):.0f})"


def main():
    no_reload = "--no-reload" in sys.argv
    numbers = [a for a in sys.argv[1:] if a != "--no-reload"]
    rounds = int(numbers[0]) if numbers else 5
    failed = 0
    good = ""                  # status midway through the sound of a successful round, for comparison
    for round_no in range(rounds + 1):
        if round_no and not no_reload:
            subprocess.run(["rmmod", "abox"], check=True)
            subprocess.run(["modprobe", "abox"], check=True)
            time.sleep(3)
        ok, what = alive()
        count, midway = 0, ""
        if ok:
            before, windows, midway = play_and_record()
            count, drawing = judge(before, windows)
            what += "; " + drawing
        # A few windows may be missing at the edges; fewer than six means broken or absent sound.
        passed = ok and count >= EXPECTED_WINDOWS - 2
        failed += not passed
        if passed:
            good = good or midway
        print(f"{'as is' if not round_no else f'round {round_no}'}: {'ok' if passed else 'FAILED'}; {what}", flush=True)
        if not passed:
            # The module's own lines (firmware lines have the time attached to "<").
            with open(LOG, encoding="utf-8", errors="replace") as f:
                lines = [line for line in f.read().splitlines() if "]<" not in line]
            print("\n".join(lines[-25:]), "-- while playing", midway, "-- after", status(),
                  "-- while playing in a good round", good, sep="\n", flush=True)
            if ok:
                # Is it this playback or the whole load? Play again without reloading.
                for _ in range(2):
                    before, windows, _ = play_and_record(8)
                    print("   again without reloading: " + judge(before, windows)[1], flush=True)
    print(f"failed rounds: {failed} of {rounds + 1}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
