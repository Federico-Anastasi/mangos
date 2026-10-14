#!/usr/bin/env python3
"""Writes a raw image to a phone partition over the USB network.

The image travels in 64 MB blocks, at a limited rate: each block arrives in the phone's RAM,
which computes its SHA-256 checksum and then copies it to the partition. All-zero blocks do not
travel: the phone writes them itself. Block 0 (partition table) goes last, so a half-written
image is not mounted at reboot. At the end the phone reads every block back from the partition
and the checksums are compared: the exit status is 0 only if all match.

Only the partitions listed in PARTITIONS are written: the others hold the bootloader and the
factory data, which cannot be recovered. The partition is given by name and the phone finds it
as the ramdisk does (PARTNAME in /sys/class/block).

Usage: push_image.py <image> <partition> [MB/s] [indexes of the blocks to resend...]
The phone must be stopped in the debug shell, with a tmpfs mounted on /mt.
"""
import hashlib
import os
import re
import socket
import subprocess
import sys
import time

HOST = os.environ.get("PHONE_HOST", "172.16.42.1")
PARTITIONS = ("boot", "recovery", "up_param", "userdata")
CHUNK_MB = 64
CHUNK = CHUNK_MB * 1024 * 1024
BASE_PORT = 10000
HERE = os.path.dirname(os.path.abspath(__file__))


def phone(cmds, wait=3):
    out = subprocess.run(["bash", os.path.join(HERE, "phone_sh.sh"), str(wait)],
                         input=cmds, capture_output=True, text=True).stdout
    return out


def find_partition(name):
    out = phone(
        "for u in /sys/class/block/*/uevent; do\n"
        f"  grep -q '^PARTNAME={name}$' $u && echo DEVICE=/dev/$(basename $(dirname $u))\n"
        "done\n")
    found = [r.split("=", 1)[1] for r in out.splitlines() if r.startswith("DEVICE=")]
    if len(found) != 1 or not re.fullmatch(r"/dev/[a-z0-9]+", found[0]):
        sys.exit(f"partition {name}: the phone did not report exactly one ({found})")
    return found[0]


def poll_log(path, seconds):
    """The text of a log on the phone, read until it holds the line "end" or the time is up."""
    log = ""
    for _ in range(seconds // 3 + 1):
        log = phone(f"cat {path}", wait=2)
        if "\nend" in "\n" + log:
            break
        time.sleep(1)
    return log


def checksums(log):
    """From the lines "block checksum [status]" of a log: {block: (checksum, status)}."""
    got = {}
    for line in log.splitlines():
        p = line.split()
        if len(p) in (2, 3) and p[0].isdigit() and len(p[1]) == 64:
            got[int(p[0])] = (p[1], p[2] if len(p) == 3 else "0")
    return got


def send(port, data, rate):
    deadline = time.time() + 60
    while True:
        try:
            s = socket.create_connection((HOST, port), timeout=5)
            break
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.5)
    s.settimeout(60)
    step = 256 * 1024
    start = time.time()
    for off in range(0, len(data), step):
        s.sendall(data[off:off + step])
        ahead = (off + step) / rate - (time.time() - start)
        if ahead > 0:
            time.sleep(ahead)
    s.shutdown(socket.SHUT_WR)
    try:
        while s.recv(4096):
            pass
    except OSError:
        pass
    s.close()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img, name = sys.argv[1], sys.argv[2]
    if name not in PARTITIONS:
        sys.exit(f"partition not allowed: {name} (allowed: {', '.join(PARTITIONS)})")
    rate = float(sys.argv[3]) * 1024 * 1024 if len(sys.argv) > 3 else 10 * 1024 * 1024
    only = [int(x) for x in sys.argv[4:]]
    dev = find_partition(name)
    size = os.path.getsize(img)
    n = (size + CHUNK - 1) // CHUNK

    sums, zeros, data_idx = {}, [], []
    with open(img, "rb") as f:
        for i in range(n):
            buf = f.read(CHUNK)
            if only and i not in only:
                continue
            if not any(buf):
                zeros.append((i, len(buf)))
            else:
                sums[i] = hashlib.sha256(buf).hexdigest()
                data_idx.append(i)
    data_idx.sort(key=lambda i: (i == 0, i))  # block 0 last
    print(f"{name} ({dev}): {n} blocks, {len(data_idx)} to send, {len(zeros)} all zeros", flush=True)

    daemon = (
        "cat > /push.sh <<'EOS'\n"
        "for i in \"$@\"; do\n"
        f"  nc -l -p $(({BASE_PORT}+i)) > /mt/c\n"
        "  s=$(sha256sum < /mt/c | cut -d' ' -f1)\n"
        f"  dd if=/mt/c of={dev} bs=1M seek=$((i*{CHUNK_MB})) 2>/dev/null\n"
        "  echo \"$i $s $?\" >> /push.log\n"
        "done\n"
        "sync; echo end >> /push.log\n"
        "EOS\n"
        "rm -f /push.log /mt/c\n"
        f"(sh /push.sh {' '.join(map(str, data_idx))} >/push.out 2>&1 </dev/null &)\n"
    )
    daemon += "zfail=\n"
    for i, ln in zeros:
        daemon += f"dd if=/dev/zero of={dev} bs=1M count={ln // 1048576} seek={i * CHUNK_MB} 2>/dev/null || zfail=1\n"
    daemon += "[ -z \"$zfail\" ] && echo zeros_done\n"
    out = phone(daemon, wait=max(3, len(zeros)))
    zeros_failed = bool(zeros) and "zeros_done" not in out.replace("echo zeros_done", "")
    if zeros_failed:
        print("zero writes not confirmed", file=sys.stderr)

    with open(img, "rb") as f:
        for k, i in enumerate(data_idx):
            f.seek(i * CHUNK)
            send(BASE_PORT + i, f.read(CHUNK), rate)
            if k % 8 == 7 or k == len(data_idx) - 1:
                print(f"sent {k + 1}/{len(data_idx)}", flush=True)

    got = checksums(poll_log("/push.log", 60))
    bad = [i for i in data_idx if got.get(i) != (sums[i], "0")]
    print("blocks bad or missing on arrival:", bad if bad else "none")

    # Every block read back from the partition, not from the phone's RAM.
    expected = dict(sums)
    zero_sums = {}
    for i, ln in zeros:
        expected[i] = zero_sums.setdefault(ln, hashlib.sha256(bytes(ln)).hexdigest())
    phone(
        "cat > /verify.sh <<'EOS'\n"
        "sync; echo 3 > /proc/sys/vm/drop_caches\n"
        "size=$1; shift\n"
        "for i in \"$@\"; do\n"
        f"  len=$((size - i * {CHUNK})); [ $len -le {CHUNK} ] || len={CHUNK}\n"
        f"  s=$(dd if={dev} bs=1M skip=$((i*{CHUNK_MB})) count={CHUNK_MB} 2>/dev/null | head -c $len | sha256sum | cut -d' ' -f1)\n"
        "  echo \"$i $s\" >> /verify.log\n"
        "done\n"
        "echo end >> /verify.log\n"
        "EOS\n"
        "rm -f /verify.log\n"
        f"(sh /verify.sh {size} {' '.join(map(str, sorted(expected)))} >/dev/null 2>&1 </dev/null &)\n")
    read_back = checksums(poll_log("/verify.log", 60 + 5 * len(expected)))
    wrong = [i for i in sorted(expected) if read_back.get(i, (None,))[0] != expected[i]]
    print("blocks that differ in the partition:", wrong if wrong else "none")
    return 1 if bad or wrong or zeros_failed else 0


if __name__ == "__main__":
    sys.exit(main())
