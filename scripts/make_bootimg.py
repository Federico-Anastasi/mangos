#!/usr/bin/env python3
"""Assembles the Android boot image (header v2) for the M13.

Reuses the header and dtb section of the image published for the A13 (addresses and format the
Samsung bootloader accepts) and replaces the "kernel" with the given uniLoader.

Usage: make_bootimg.py <reference A13 image> <uniLoader> <output image>
"""
import struct
import sys

PAGE = 2048


def pad(b):
    return b + b"\0" * (-len(b) % PAGE)


def main():
    ref_path, kernel_path, out_path = sys.argv[1:4]
    ref = open(ref_path, "rb").read()
    kernel = open(kernel_path, "rb").read()

    assert ref[:8] == b"ANDROID!"
    ksize, _, rsize, _, ssize, _, _, page, hver, _ = struct.unpack("<10I", ref[8:48])
    rdo_size = struct.unpack("<I", ref[1632:1636])[0]
    dtb_size = struct.unpack("<I", ref[1648:1652])[0]
    assert (page, hver, rsize, ssize, rdo_size) == (PAGE, 2, 0, 0, 0), "unexpected reference header"
    dtb_off = PAGE + len(pad(ref[PAGE:PAGE + ksize]))
    dtb = ref[dtb_off:dtb_off + dtb_size]

    header = bytearray(ref[:PAGE])
    header[8:12] = struct.pack("<I", len(kernel))
    out = bytes(header) + pad(kernel) + pad(dtb)
    open(out_path, "wb").write(out)
    print(f"{out_path}: {len(out)} bytes (kernel {len(kernel)}, dtb {dtb_size})")


if __name__ == "__main__":
    main()
