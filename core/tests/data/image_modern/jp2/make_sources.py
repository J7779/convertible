#!/usr/bin/env python3
# Writes the synthetic source pictures the JPEG 2000 fixtures are encoded from
# (standard library only). Run from this directory: python3 -I make_sources.py
import struct
import zlib


def lcg(seed):
    state = seed
    while True:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        yield state >> 16


def rgb_pattern(w, h, seed=7):
    noise = lcg(seed)
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // (w - 1)
            g = (y * 255) // (h - 1)
            b = 255 - (x + y) * 255 // (w + h - 2)
            if (x // 8 + y // 8) % 2 == 0:
                b = (b + 96) % 256
            if 10 <= x < 22 and 12 <= y < 24:
                r, g, b = 250, 30, 30
            n = next(noise) % 9 - 4
            out += bytes(max(0, min(255, v + n)) for v in (r, g, b))
    return out


def write_ppm(path, w, h, data):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(data)


def write_pgm16(path, w, h, values):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n65535\n" % (w, h))
        f.write(b"".join(struct.pack(">H", v) for v in values))


def write_pgm8(path, w, h, values):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes(values))


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def write_png_rgba(path, w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(png_chunk(b"IEND", b""))


def main():
    # 8-bit RGB, 48x40.
    w, h = 48, 40
    write_ppm("src_rgb.ppm", w, h, rgb_pattern(w, h))

    # 8-bit RGBA with a soft alpha ramp and a fully transparent corner, 32x24.
    w, h = 32, 24
    rgb = rgb_pattern(w, h, seed=3)
    rgba = bytearray()
    for i in range(w * h):
        x, y = i % w, i // w
        a = 0 if (x < 6 and y < 6) else min(255, 40 + x * 7 + y * 2)
        rgba += rgb[i * 3:i * 3 + 3] + bytes([a])
    write_png_rgba("src_rgba.png", w, h, rgba)

    # 16-bit greyscale ramp with detail in the low bits, 40x32.
    w, h = 40, 32
    noise = lcg(5)
    gray16 = [min(65535, (x * 65535 // (w - 1) + y * 517 + next(noise) % 251) % 65536) for y in range(h) for x in range(w)]
    write_pgm16("src_gray16.pgm", w, h, gray16)

    # Signed 12-bit samples (-2048..2047), big-endian 16-bit words, 32x24.
    w, h = 32, 24
    signed = [((x * 128 - 2048) + (y * 37) % 200 - 100) for y in range(h) for x in range(w)]
    signed = [max(-2048, min(2047, v)) for v in signed]
    with open("src_s12_32x24.raw", "wb") as f:
        f.write(b"".join(struct.pack(">h", v) for v in signed))

    # YCbCr 4:2:0, planar: Y 48x40 then Cb and Cr 24x20 each.
    w, h = 48, 40
    rgb = rgb_pattern(w, h, seed=9)
    ys, cbs, crs = [], [], []
    for i in range(w * h):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        ys.append(max(0, min(255, round(0.299 * r + 0.587 * g + 0.114 * b))))
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            acc = [0.0, 0.0, 0.0]
            for dy in (0, 1):
                for dx in (0, 1):
                    i = (y + dy) * w + x + dx
                    for c in range(3):
                        acc[c] += rgb[i * 3 + c] / 4
            r, g, b = acc
            cbs.append(max(0, min(255, round(128 - 0.168736 * r - 0.331264 * g + 0.5 * b))))
            crs.append(max(0, min(255, round(128 + 0.5 * r - 0.418688 * g - 0.081312 * b))))
    with open("src_ycc420_48x40.raw", "wb") as f:
        f.write(bytes(ys) + bytes(cbs) + bytes(crs))

    # Palette indices 0..15 (stripes and blocks), 40x24.
    w, h = 40, 24
    write_pgm8("src_index.pgm", w, h, [((x // 5) + (y // 6) * 3) % 16 for y in range(h) for x in range(w)])


if __name__ == "__main__":
    main()
