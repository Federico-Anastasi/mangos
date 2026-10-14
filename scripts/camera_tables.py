#!/usr/bin/env python3
"""Extract from the kernel drivers the register tables of the camera sensors (S5KJN1 rear,
GC08A3 front) and write them to mangos/tools/camera/<sensor>_tables.h, in the form our module
uses. Only needs redoing if the kernel sources change.

Usage, from WSL: python3 scripts/camera_tables.py [kernel sources; default $W/linux-wifi]
"""
import os
import re
import sys

W = os.environ.get("W", os.path.expanduser("~/m13"))
KERNEL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(W, "linux-wifi")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# For each sensor: the driver, its copyright, how its register names start, how many bits a
# value has, and the tables to take with the name they will have in our module.
SENSORS = {
    "s5kjn1": ("drivers/media/i2c/s5kjn1.c", "Copyright (c) 2025 Linaro Ltd", "S5KJN1_REG_", 16,
               (("init_array_setting", "s5kjn1_init"), ("s5kjn1_4080x3072_30fps_mode", "s5kjn1_4080x3072"))),
    "gc08a3": ("drivers/media/i2c/gc08a3.c", "Copyright (c) 2024 MediaTek Inc.", "GC08A3_", 8,
               (("mode_table_common", "gc08a3_init"), ("mode_3264x2448", "gc08a3_3264x2448"))),
}

for sensor, (driver, author, prefix, bits, tables) in SENSORS.items():
    source = open(f"{KERNEL}/{driver}", encoding="utf-8").read()
    names = dict(re.findall(r"#define\s+(%s\w+)\s+(CCI_REG(?:8|16)\(0x[0-9a-fA-F]+\))" % prefix, source))
    lines = ["/* SPDX-License-Identifier: GPL-2.0-only */",
             "/*",
             f" * Register tables of sensor {sensor.upper()}, derived from {driver} of the Linux",
             f" * kernel ({author}) with scripts/camera_tables.py. Do not edit by hand.",
             f" * Every value is {bits} bits. They are computed for a 24 MHz clock: the module then corrects",
             " * the clock-dependent registers, because on the M13 it is 26 MHz.",
             " */",
             ""]
    for name, ours in tables:
        body = re.search(r"static const struct cci_reg_sequence %s\[\] = \{(.*?)\n\};" % name, source, re.S)
        if not body:
            sys.exit(f"table {name} not found in {KERNEL}/{driver}")
        # Without the comments, which may contain braces.
        text = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
        table_rows = text.count("{")
        # Some entries name the register instead of writing its address.
        text = re.sub(r"\b%s\w+" % prefix, lambda m: names.get(m.group(0), m.group(0)), text)
        entries = re.findall(r"\{\s*CCI_REG(8|16)\((0x[0-9a-fA-F]+)\),\s*(0x[0-9a-fA-F]+)\s*\}", text)
        if len(entries) != table_rows or any(int(wide) != bits for wide, _, _ in entries):
            sys.exit(f"{name}: {table_rows} rows but {len(entries)} {bits}-bit entries read: the format has changed")
        lines.append(f"static const struct reg_entry {ours}[] = {{")
        lines += [f"\t{{ {reg.lower()}, {value.lower()} }}," for _, reg, value in entries]
        lines += ["};", ""]
        print(f"{ours}: {len(entries)} entries")
    with open(f"{REPO}/mangos/tools/camera/{sensor}_tables.h", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
