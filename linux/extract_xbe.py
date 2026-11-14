#!/usr/bin/env python3
# Extracts a file from an original Xbox disc image (xdvdfs), the same way the runtime's kernel/Disc mounts one:
# the game partition is at 0 (a plain xiso) or past the video partition at 0x18300000, the volume descriptor sits
# at sector 32 of it, and each folder is a binary tree of entries (+0 left, +2 right in 4-byte units, +4 sector,
# +8 size, +0xC attributes, +0xD name length, +0xE name). Used to pull default.xbe from the player's .iso so the
# build can bake its sha-1 and game discovery takes the extracted-files path.
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = [0, 0x18300000]


def read_tree(f, base, offset, size, want, out):
    f.seek(offset)
    table = f.read(size)
    if len(table) != size:
        raise SystemExit("short read on a folder table")
    seen = set()
    todo = [0]
    while todo:
        at = todo.pop()
        if at + 0xE > size or at // 4 in seen:
            continue
        seen.add(at // 4)
        left, right = struct.unpack_from("<HH", table, at)
        if left == 0xFFFF:
            continue
        sector, fsize = struct.unpack_from("<II", table, at + 4)
        attributes, length = table[at + 0xC], table[at + 0xD]
        if at + 0xE + length > size:
            continue
        if left:
            todo.append(left * 4)
        if right:
            todo.append(right * 4)
        name = table[at + 0xE : at + 0xE + length].decode("ascii", "replace")
        if name.upper() == want and not (attributes & 0x10):
            f.seek(base + sector * SECTOR)
            with open(out, "wb") as o:
                left_bytes = fsize
                while left_bytes > 0:
                    chunk = f.read(min(left_bytes, 1 << 22))
                    if not chunk:
                        raise SystemExit(f"unexpected end of image reading {name}")
                    o.write(chunk)
                    left_bytes -= len(chunk)
            print(f"extracted {name} ({fsize} bytes, sector {sector}) -> {out}")
            return True
        if attributes & 0x10 and name not in (".", ".."):
            if read_tree(f, base, base + sector * SECTOR, fsize, want, out):
                return True
    return False


iso, want, out = sys.argv[1], sys.argv[2].upper(), sys.argv[3]
with open(iso, "rb") as f:
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        vd = f.read(SECTOR)
        if len(vd) == SECTOR and vd[:20] == MAGIC and vd[0x7EC : 0x7EC + 20] == MAGIC:
            sector, size = struct.unpack_from("<II", vd, 0x14)
            if read_tree(f, base, base + sector * SECTOR, size, want, out):
                sys.exit(0)
            raise SystemExit(f"{sys.argv[2]} not found in the image")
    raise SystemExit("no Xbox game partition found (tried a plain xiso and a whole-disc dump)")
