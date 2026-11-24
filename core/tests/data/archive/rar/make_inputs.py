#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Synthetic inputs for the RAR filter and text fixtures (see SOURCES.md):
#     python3 make_inputs.py <directory>
import math, os, struct, sys

def xorshift(seed):
    s = seed
    while True:
        s ^= (s << 13) & 0xFFFFFFFF; s ^= s >> 17; s ^= (s << 5) & 0xFFFFFFFF
        yield s

def x86(n):
    # Instruction-like bytes with many relative CALL (E8) and JMP (E9) to a few targets.
    rnd = xorshift(12345)
    out = bytearray()
    targets = [0x400, 0x1800, 0x2C00, 0x3A00, 0x5000]
    ops = [b"\x55", b"\x89\xe5", b"\x83\xec\x10", b"\x8b\x45\x08", b"\x01\xd0", b"\x5d", b"\xc3", b"\x31\xc0", b"\x90"]
    while len(out) < n:
        r = next(rnd)
        if r % 5 == 0:
            t = targets[(r >> 8) % len(targets)]
            op = 0xE8 if (r >> 16) % 3 else 0xE9
            rel = (t - (len(out) + 5)) & 0xFFFFFFFF
            out += bytes([op]) + struct.pack("<I", rel)
        else:
            out += ops[(r >> 4) % len(ops)]
    return bytes(out[:n])

def rgb(w, h):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes([(x * 4) & 255, (y * 4) & 255, ((x + y) * 2) & 255])
    return bytes(out)

def bmp(w, h):
    pix = rgb(w, h)
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(pix), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pix), 2835, 2835, 0, 0)
    return hdr + dib + pix

def wav(samples):
    data = bytearray()
    for i in range(samples):
        v = int(9000 * math.sin(i * 2 * math.pi * 440 / 8000) + 3000 * math.sin(i * 2 * math.pi * 1250 / 8000))
        data += struct.pack("<hh", v, -v // 2)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 8000, 32000, 4, 16)
    return hdr + b"data" + struct.pack("<I", len(data)) + bytes(data)

def table(n):
    out = bytearray()
    for i in range(n):
        out += struct.pack("<IHH", 100000 + i * 37, (i * 7) & 0xFFFF, 0x4000 + (i % 50))
    return bytes(out)

LOREM = ("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
         "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
         "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
         "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
         "officia deserunt mollit anim id est laborum.\n")

def text(n):
    words = LOREM.split()
    rnd = xorshift(777)
    out = []
    size = 0
    while size < n:
        w = words[next(rnd) % len(words)]
        out.append(w)
        size += len(w) + 1
    return (" ".join(out)[:n]).encode()

def noise(n, seed):
    rnd = xorshift(seed)
    return bytes(next(rnd) & 0xFF for _ in range(n))

def far():
    # One random block repeated across long stretches of zeros: matches 300 KB to 2.5 MB back.
    seg = noise(8192, 31337)
    return seg + bytes(300000) + seg + bytes(1000000) + seg + bytes(2500000) + seg

def wrap():
    # A random block every 40000 bytes over 1 MB: wraps a small dictionary many times.
    seg = noise(6000, 4711)
    return b"".join(seg + bytes(34000) + bytes([i]) * 16 for i in range(26))

def sparse_x86(chunks):
    # Mostly zeros with a CALL and a JMP per KiB to four absolute targets: filter
    # blocks far into the file (non-zero file offsets) that still compress well.
    out = bytearray()
    for i in range(chunks):
        chunk = bytearray(1024)
        for at, op in ((100, 0xE8), (600, 0xE9)):
            pos = i * 1024 + at
            target = 0x10000 * (1 + (i + at) % 4)
            chunk[at] = op
            chunk[at + 1:at + 5] = struct.pack("<I", (target - (pos + 5)) & 0xFFFFFFFF)
        out += chunk
    return bytes(out)

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
files = {"x86.bin": x86(24576), "rgb.bmp": bmp(64, 64), "pcm.wav": wav(6000), "table.dat": table(3000), "text.txt": text(20000),
         "far.bin": far(), "wrap.bin": wrap(),
         "sparse.exe": sparse_x86(640)}
for name, data in files.items():
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)
    os.utime(os.path.join(d, name), (1623758400, 1623758400))
