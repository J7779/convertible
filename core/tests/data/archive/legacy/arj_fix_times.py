#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
"""Replace the archive-creation and file-creation timestamps that ARJ takes from the
clock (main header created/modified, file header DTC) with the tree's fixed time and
recompute each header's CRC-32. Usage: arj_fix_times.py file.arj [now_unix]"""
import struct, sys, time, zlib

FIXED = 1623758400
path = sys.argv[1]
now = int(sys.argv[2]) if len(sys.argv) > 2 else int(time.time())
d = bytearray(open(path, 'rb').read())


def recent(v):
    return now - 30 * 86400 < v < now + 86400


pos = 0
first = True
while True:
    assert d[pos:pos + 2] == b'\x60\xea', hex(pos)
    size = struct.unpack_from('<H', d, pos + 2)[0]
    if size == 0:
        break
    h = pos + 4
    fhs = d[h]
    slots = [8, 12] if first else [8]
    if not first and fhs >= 46:
        slots += [34, 38]
    for s in slots:
        v = struct.unpack_from('<I', d, h + s)[0]
        if recent(v):
            struct.pack_into('<I', d, h + s, FIXED)
    struct.pack_into('<I', d, h + size, zlib.crc32(bytes(d[h:h + size])) & 0xFFFFFFFF)
    p = h + size + 4
    while True:  # extended headers
        ext = struct.unpack_from('<H', d, p)[0]
        p += 2
        if ext == 0:
            break
        p += ext + 4
    if not first:
        p += struct.unpack_from('<I', d, h + 12)[0]  # compressed size
    pos = p
    first = False
open(path, 'wb').write(d)
