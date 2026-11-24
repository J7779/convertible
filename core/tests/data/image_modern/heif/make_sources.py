# Writes the synthetic PNG test patterns the HEIF/AVIF fixtures are encoded from.
# Standard library only. Usage: python3 make_sources.py <output-directory>
import math
import os
import struct
import sys
import zlib


def png(path, width, height, rows, color_type, bit_depth, icc=None):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0))
    if icc is not None:
        out += chunk(b"iCCP", b"test\x00\x00" + zlib.compress(icc, 9))
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def pattern(x, y, w, h):
    """Smooth colour ramps plus a few hard-edged blocks: a photo-like test card."""
    r = x * 255 // max(1, w - 1)
    g = y * 255 // max(1, h - 1)
    b = (x + y) * 255 // max(1, w + h - 2)
    if (x // 8 + y // 8) % 5 == 0:
        r, g, b = 255 - r, 255 - g, 255 - b
    if w // 3 <= x < w // 3 + 6 and h // 4 <= y < h // 4 + 6:
        r, g, b = 255, 255, 0
    return r, g, b


def rgb8(w, h):
    return [bytes(c for x in range(w) for c in pattern(x, y, w, h)) for y in range(h)]


def rgba8(w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = pattern(x, y, w, h)
            # A soft radial alpha with a fully transparent corner and an opaque centre.
            d = math.hypot(x - w / 2, y - h / 2) / math.hypot(w / 2, h / 2)
            a = 0 if (x < 6 and y < 6) else max(0, min(255, int(round(255 * (1.2 - d)))))
            row += bytes((r, g, b, a))
        rows.append(bytes(row))
    return rows


def rgb16(w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r = x * 65535 // max(1, w - 1)
            g = y * 65535 // max(1, h - 1)
            b = (x * 7 + y * 13) * 65535 // max(1, (w - 1) * 7 + (h - 1) * 13)
            row += struct.pack(">HHH", r, g, b)
        rows.append(bytes(row))
    return rows


def tiny_icc():
    """A small matrix/TRC display profile (ICC v2): sRGB-like primaries, gamma 2.2."""
    def xyz(x, y, z):
        return b"XYZ " + b"\x00" * 4 + struct.pack(">iii", int(x * 65536), int(y * 65536), int(z * 65536))

    def text_tag(text):
        data = text.encode("ascii") + b"\x00"
        return b"desc" + b"\x00" * 4 + struct.pack(">I", len(data)) + data + b"\x00" * (4 + 4 + 2 + 1 + 67)

    curve = b"curv" + b"\x00" * 4 + struct.pack(">IH", 1, int(2.2 * 256)) + b"\x00\x00"
    tags = [
        (b"desc", text_tag("Test RGB")),
        (b"cprt", b"text" + b"\x00" * 4 + b"No copyright, use freely\x00\x00\x00\x00"),
        (b"wtpt", xyz(0.9642, 1.0, 0.8249)),
        (b"rXYZ", xyz(0.4361, 0.2225, 0.0139)),
        (b"gXYZ", xyz(0.3851, 0.7169, 0.0971)),
        (b"bXYZ", xyz(0.1431, 0.0606, 0.7141)),
        (b"rTRC", curve),
        (b"gTRC", curve),
        (b"bTRC", curve),
    ]
    table = struct.pack(">I", len(tags))
    data = b""
    offset = 128 + 4 + 12 * len(tags)
    for sig, body in tags:
        while len(body) % 4:
            body += b"\x00"
        table += sig + struct.pack(">II", offset + len(data), len(body))
        data += body
    size = 128 + len(table) + len(data)
    header = bytearray(128)
    struct.pack_into(">I", header, 0, size)
    struct.pack_into(">I", header, 8, 0x02100000)
    header[12:16] = b"mntr"
    header[16:20] = b"RGB "
    header[20:24] = b"XYZ "
    struct.pack_into(">HHHHHH", header, 24, 2020, 1, 1, 0, 0, 0)
    header[36:40] = b"acsp"
    struct.pack_into(">iii", header, 68, 63190, 65536, 54061)
    return bytes(header) + table + data


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    png(os.path.join(out, "card_64x48.png"), 64, 48, rgb8(64, 48), 2, 8)
    png(os.path.join(out, "card_80x48.png"), 80, 48, rgb8(80, 48), 2, 8)
    png(os.path.join(out, "card_33x21.png"), 33, 21, rgb8(33, 21), 2, 8)
    # The 80x48 card scaled up by point sampling: large enough for Apple's 512x512 tiling.
    big = [bytes(c for x in range(1030) for c in pattern(x * 80 // 1030, y * 48 // 520, 80, 48)) for y in range(520)]
    png(os.path.join(out, "card_1030x520.png"), 1030, 520, big, 2, 8)
    png(os.path.join(out, "card_icc_32x24.png"), 32, 24, rgb8(32, 24), 2, 8, icc=tiny_icc())
    png(os.path.join(out, "alpha_48x32.png"), 48, 32, rgba8(48, 32), 6, 8)
    png(os.path.join(out, "ramp16_48x32.png"), 48, 32, rgb16(48, 32), 2, 16)
    for i, shift in enumerate((0, 16, 32)):
        rows = [bytes(c for x in range(32) for c in pattern((x + shift) % 32, y, 32, 24)) for y in range(24)]
        png(os.path.join(out, "seq_%d.png" % (i + 1)), 32, 24, rows, 2, 8)


if __name__ == "__main__":
    main()
