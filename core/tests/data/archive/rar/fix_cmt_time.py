#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Set the modification time of the comment (NEWSUB "CMT") block of a RAR 4 archive to
# 2021-06-15 12:00:00 and recompute its header CRC (rar stamps the current time there).
import struct, sys, zlib

path = sys.argv[1]
d = bytearray(open(path, "rb").read())
dos = (((2021 - 1980) << 9 | 6 << 5 | 15) << 16) | (12 << 11)
p = 7
while p + 7 <= len(d):
    crc, kind, flags, size = struct.unpack_from("<HBHH", d, p)
    add = struct.unpack_from("<I", d, p + 7)[0] if (kind in (0x74, 0x7A) or flags & 0x8000) else 0
    if kind == 0x7A:
        name_size = struct.unpack_from("<H", d, p + 26)[0]
        name_at = p + 32 + (8 if flags & 0x100 else 0)
        if d[name_at:name_at + name_size] == b"CMT":
            struct.pack_into("<I", d, p + 20, dos)
            struct.pack_into("<H", d, p, zlib.crc32(bytes(d[p + 2:p + size])) & 0xFFFF)
            print("patched CMT at", p)
    if kind == 0x7B:
        break
    p += size + add
open(path, "wb").write(d)
