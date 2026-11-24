#!/usr/bin/env python3
"""Re-store the tiles of a GIMP XCF file (RLE) as zlib or uncompressed tiles.

Usage: python3 recompress_xcf.py <in.xcf> <out.xcf> zlib|none

The structure GIMP wrote is kept byte for byte; every tile is decoded from
GIMP's RLE, stored again at the end of the file (zlib via Python's zlib
module, or raw), the tile pointers are redirected to the new copies and the
PROP_COMPRESSION byte is updated. Python standard library only.
"""
import struct
import sys
import zlib

src, dst, mode = sys.argv[1], sys.argv[2], sys.argv[3]
data = bytearray(open(src, "rb").read())
assert data[:9] == b"gimp xcf "
version = 0 if data[9:13] == b"file" else int(data[10:13])
wide = version >= 11
psize = 8 if wide else 4
out = bytearray(data)


def u32(pos):
    return struct.unpack(">I", data[pos:pos + 4])[0]


def ptr(pos):
    return struct.unpack(">Q" if wide else ">I", data[pos:pos + psize])[0]


def set_ptr(pos, value):
    out[pos:pos + psize] = struct.pack(">Q" if wide else ">I", value)


def skip_props(pos, on_prop=None):
    while True:
        pid, size = struct.unpack(">II", data[pos:pos + 8])
        if on_prop:
            on_prop(pid, pos + 8, size)
        pos += 8 + size
        if pid == 0:
            return pos


def rle_decode(pos, pixels, bpp):
    planes = []
    for _ in range(bpp):
        plane = bytearray()
        while len(plane) < pixels:
            n = data[pos]
            pos += 1
            if n >= 128:
                n = 256 - n
                if n == 128:
                    n = struct.unpack(">H", data[pos:pos + 2])[0]
                    pos += 2
                plane += data[pos:pos + n]
                pos += n
            else:
                n += 1
                if n == 128:
                    n = struct.unpack(">H", data[pos:pos + 2])[0]
                    pos += 2
                plane += bytes([data[pos]]) * n
                pos += 1
        planes.append(plane)
    return bytes(b for i in range(pixels) for b in (p[i] for p in planes))


def hierarchy(pos):
    width, height, bpp = struct.unpack(">III", data[pos:pos + 12])
    level = ptr(pos + 12)
    tiles_pos = level + 8
    tx, ty = (width + 63) // 64, (height + 63) // 64
    for t in range(tx * ty):
        tw = min(64, width - (t % tx) * 64)
        th = min(64, height - (t // tx) * 64)
        raw = rle_decode(ptr(tiles_pos + t * psize), tw * th, bpp)
        stored = zlib.compress(raw, 9) if mode == "zlib" else raw
        set_ptr(tiles_pos + t * psize, len(out))
        out.extend(stored)


def compression(pid, pos, size):
    if pid == 17:
        assert data[pos] == 1, "input must be RLE"
        out[pos] = 2 if mode == "zlib" else 0


pos = skip_props(30 if version >= 4 else 26, compression)
layers = []
while ptr(pos):
    layers.append(ptr(pos))
    pos += psize
pos += psize
channels = []
while ptr(pos):
    channels.append(ptr(pos))
    pos += psize
for layer in layers:
    name_len = u32(layer + 12)
    p = skip_props(layer + 16 + name_len)
    hierarchy(ptr(p))
    if ptr(p + psize):
        channels.append(ptr(p + psize))
for channel in channels:
    name_len = u32(channel + 8)
    p = skip_props(channel + 12 + name_len)
    hierarchy(ptr(p))
open(dst, "wb").write(out)
