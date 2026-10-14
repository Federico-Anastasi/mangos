#!/usr/bin/env python3
"""Builds the up_param partition with the mangOS boot screens.

up_param is a tar archive of JPEGs that the bootloader shows at power-on. It starts from a copy
of the original and replaces only the images with brand text; the others (download mode,
charging while off, errors) stay as shipped.

Usage: build_up_param.py <original up_param> <splash.svg> <output up_param>
Requires rsvg-convert and Pillow.
"""
import io
import subprocess
import sys
import tarfile

from PIL import Image

SCREEN = (1080, 2408)


def jpeg(im):
    buf = io.BytesIO()
    # Baseline, 4:4:4, standard tables: the same format as the originals.
    im.save(buf, "JPEG", quality=92, subsampling=0, optimize=False, progressive=False)
    return buf.getvalue()


def centered_crop(im, size):
    left = (im.width - size[0]) // 2
    top = (im.height - size[1]) // 2
    return im.crop((left, top, left + size[0], top + size[1]))


def main():
    src_path, svg_path, out_path = sys.argv[1:4]
    png = subprocess.run(["rsvg-convert", "-w", str(SCREEN[0]), "-h", str(SCREEN[1]), svg_path],
                         check=True, capture_output=True).stdout
    splash = Image.open(io.BytesIO(png)).convert("RGB")
    assert splash.size == SCREEN

    src = tarfile.open(src_path)
    sizes = {m.name: Image.open(src.extractfile(m)).size for m in src.getmembers()}
    replacements = {
        "logo.jpg": jpeg(splash),
        "letter.jpg": jpeg(splash),
        # Unlocked-bootloader warning: central crop of the same screen.
        "svb_orange.jpg": jpeg(centered_crop(splash, sizes["svb_orange.jpg"])),
        # "Unofficial software" warning: black.
        "booting_warning.jpg": jpeg(Image.new("RGB", sizes["booting_warning.jpg"], (0, 0, 0))),
    }
    for name, data in replacements.items():
        assert Image.open(io.BytesIO(data)).size == sizes[name], name

    partition_size = len(open(src_path, "rb").read())
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as out:
        for m in src.getmembers():
            data = replacements.get(m.name) or src.extractfile(m).read()
            m.size = len(data)
            out.addfile(m, io.BytesIO(data))
    image = buf.getvalue()
    assert len(image) <= partition_size, "the archive does not fit in the partition"
    open(out_path, "wb").write(image + b"\0" * (partition_size - len(image)))
    print(f"{out_path}: {len(image)} bytes of archive in {partition_size}, replaced {sorted(replacements)}")


if __name__ == "__main__":
    main()
