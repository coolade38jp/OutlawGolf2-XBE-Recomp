#!/usr/bin/env python3
# Writes a stub image_symbols.h for builds without the game's xbe: same output shape as the recompiler's
# (sdk/tools/recomp/codegen_emit.cpp), from config/game.toml's [symbols], [data] and the XDK build's [layout].
# Symbols at 0 (absent from the image) get the recompiler's own placeholder addresses, so the host override still
# links; a release build bakes the real generated file instead, by running the recompiler on the xbe.
import re
import sys
from pathlib import Path

config_path, xdk_dir, out_path = sys.argv[1], Path(sys.argv[2]), sys.argv[3]

section = None
symbols = []   # (name, value or None)
data = []
xdk_build = None
xbe_name = "default.xbe"
abi_names = []
for line in Path(config_path).read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    m = re.match(r"\[([A-Za-z0-9_]+)\]", line)
    if m:
        section = m.group(1)
        continue
    if section == "xbe":
        m = re.match(r"xdk\s*=\s*(\d+)", line)
        if m:
            xdk_build = m.group(1)
        m = re.match(r'path\s*=\s*"([^"]+)"', line)
        if m:
            xbe_name = Path(m.group(1)).name
    elif section in ("symbols", "data"):
        m = re.match(r'([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)', line)
        if m:
            value = int(m.group(2), 0)
            (symbols if section == "symbols" else data).append((m.group(1), value or None))
    elif section == "abi":
        m = re.match(r"([A-Za-z0-9_]+)\s*=\s*\[([^\]]*)\]", line)
        if m:
            abi_names.append((m.group(1), [x.strip() for x in m.group(2).split(",") if x.strip()]))

lines = ["#pragma once", "// a build without the game's xbe: from config/game.toml by linux/gen_image_symbols.py"]
lines.append(f'#define IMAGE_XBE "{xbe_name}"')
absent = 0
for name, value in symbols + data:
    if value is None:
        value = 0xFFFF0000 + absent
        absent += 1
        has = 0
    else:
        has = 1
    lines.append(f"#define SYM_{name} {value:08X}")
    lines.append(f"#define HAS_{name} {has}")

layout = Path(xdk_dir) / xdk_build / "layout.toml"
if layout.is_file():
    in_layout = False
    for line in layout.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        m = re.match(r"\[([A-Za-z0-9_]+)\]", line)
        if m:
            in_layout = m.group(1) == "layout"
            continue
        m = re.match(r"([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", line)
        if in_layout and m:
            lines.append(f"#define XDK_{m.group(1)} 0x{int(m.group(2), 0):X}")

if abi_names:
    rows = "".join(" \\\n\tX(" + name + (", " + ", ".join(locs) if locs else "") + ")" for name, locs in abi_names)
    lines.append("#define IMAGE_ABI(X)" + rows)

Path(out_path).write_text("\n".join(lines) + "\n")
print(f"image_symbols.h: {len(symbols)} symbols, {len(data)} data, {absent} absent placeholders")
