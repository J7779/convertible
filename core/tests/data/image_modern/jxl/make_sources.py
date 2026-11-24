#!/usr/bin/env python3
# Writes the synthetic source pictures the JPEG XL fixtures are encoded from
# (Python standard library only). Run from this directory:
#   python3 make_sources.py
# then make_fixtures.sh encodes them with the reference libjxl tools.
import struct
import zlib

W, H = 64, 48


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def png(path, width, height, color_type, depth, rows, extra=b""):
    """rows: list of bytes objects (one per row, without filter byte)."""
    raw = b"".join(b"\0" + r for r in rows)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0))
    data += extra
    data += chunk(b"IDAT", zlib.compress(raw, 9))
    data += chunk(b"IEND", b"")
    open(path, "wb").write(data)


class Lcg:
    def __init__(self, seed):
        self.s = seed

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return self.s >> 16


def rgb_pixel(x, y, rnd):
    r = (x * 255) // (W - 1)
    g = (y * 255) // (H - 1)
    b = ((x + y) * 4) & 255
    if 8 <= x < 24 and 8 <= y < 20:
        r, g, b = 220, 40, 30
    if 36 <= x < 56 and 26 <= y < 42:
        r, g, b = 20, 60, 200
    n = rnd.next() % 9 - 4
    return [max(0, min(255, v + n)) for v in (r, g, b)]


def make_rgb():
    rnd = Lcg(7)
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            row += rgb_pixel(x, y, rnd)
        rows.append(bytes(row))
    png("src_rgb.png", W, H, 2, 8, rows)
    # Same pixels as binary PPM for cjpeg (deleted again by make_fixtures.sh).
    open("src_rgb.ppm", "wb").write(b"P6\n%d %d\n255\n" % (W, H) + b"".join(rows))


def make_rgba():
    rnd = Lcg(9)
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            r, g, b = rgb_pixel(x, y, rnd)
            a = (x * 255) // (W - 1)
            if y < 6:
                a = 0
            row += [r, g, b, a]
        rows.append(bytes(row))
    png("src_rgba.png", W, H, 6, 8, rows)


def make_grey():
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            v = (x * 4 + y * 2) & 255
            if ((x // 8) + (y // 8)) % 2 == 0:
                v = 255 - v
            row.append(v)
        rows.append(bytes(row))
    png("src_grey.png", W, H, 0, 8, rows)


def make_rgb16():
    w, h = 40, 30
    rows = []
    for y in range(h):
        row = b""
        for x in range(w):
            for c in range(3):
                row += struct.pack(">H", (x * 2039 + y * 977 + c * 12345) % 65536)
        rows.append(row)
    png("src_rgb16.png", w, h, 2, 16, rows)


def make_pfm():
    w, h = 32, 24
    # PFM rows are stored bottom to top, little-endian when the scale is negative.
    out = b"PF\n%d %d\n-1.0\n" % (w, h)
    for y in reversed(range(h)):
        for x in range(w):
            r = 4.0 * x / (w - 1)
            g = 2.0 * y / (h - 1)
            b = 0.25 + 0.5 * ((x + y) % 2)
            out += struct.pack("<fff", r, g, b)
    open("src_float.pfm", "wb").write(out)


def tiff_exif(orientation):
    """A tiny little-endian TIFF/EXIF block: Orientation, Software, ImageDescription."""
    software = b"Convertible test\0"
    description = b"Test pattern\0"
    count = 3
    ifd_size = 2 + count * 12 + 4
    data_offset = 8 + ifd_size
    out = b"II*\0" + struct.pack("<I", 8) + struct.pack("<H", count)
    out += struct.pack("<HHIHH", 0x010E, 2, len(description), data_offset, 0)
    out += struct.pack("<HHIHH", 0x0112, 3, 1, orientation, 0)
    out += struct.pack("<HHII", 0x0131, 2, len(software), data_offset + len(description))
    out += struct.pack("<I", 0)
    out += description + software
    return out


XMP = (b'<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>'
       b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
       b'<rdf:Description xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Test pattern</dc:title>'
       b'</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>')


def make_exif_png():
    rnd = Lcg(7)
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            row += rgb_pixel(x, y, rnd)
        rows.append(bytes(row))
    extra = chunk(b"eXIf", tiff_exif(1))
    extra += chunk(b"iTXt", b"XML:com.adobe.xmp\0\0\0\0\0" + XMP)
    png("src_exif.png", W, H, 2, 8, rows, extra)


def make_orient_png():
    # Asymmetric picture (red top-left marker) stored with EXIF orientation 6.
    w, h = 40, 24
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            if x < 8 and y < 8:
                row += [255, 0, 0]
            else:
                row += [(x * 6) & 255, (y * 10) & 255, 128]
        rows.append(bytes(row))
    png("src_orient6.png", w, h, 2, 8, rows, chunk(b"eXIf", tiff_exif(6)))


def make_anim():
    # APNG: 3 frames of 32x32, a white square moving over blue; delays 100/200/300 ms.
    w = h = 32
    frames = []
    for i in range(3):
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                inside = 4 + i * 8 <= x < 12 + i * 8 and 10 <= y < 18
                row += [255, 255, 255] if inside else [10, 40 + i * 60, 200]
            rows.append(bytes(row))
        frames.append(rows)
    delays = [100, 200, 300]
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"acTL", struct.pack(">II", 3, 0))
    seq = 0
    for i, rows in enumerate(frames):
        data += chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, w, h, 0, 0, delays[i], 1000, 0, 0))
        seq += 1
        z = zlib.compress(b"".join(b"\0" + r for r in rows), 9)
        if i == 0:
            data += chunk(b"IDAT", z)
        else:
            data += chunk(b"fdAT", struct.pack(">I", seq) + z)
            seq += 1
    data += chunk(b"IEND", b"")
    open("src_anim.png", "wb").write(data)


def s15(v):
    return struct.pack(">i", int(round(v * 65536)))


def make_icc_png():
    """RGB picture tagged with a synthetic ICC v2 profile whose tone curves are
    sampled tables (not expressible as a JPEG XL colour enum)."""
    tags = []
    desc_text = b"Sampled curve test\0"
    desc = b"desc\0\0\0\0" + struct.pack(">I", len(desc_text)) + desc_text + b"\0" * 4 + b"\0" * 2 + b"\0" + b"\0" * 67
    tags.append((b"desc", desc))
    tags.append((b"cprt", b"text\0\0\0\0" + b"No copyright\0"))
    tags.append((b"wtpt", b"XYZ \0\0\0\0" + s15(0.9642) + s15(1.0) + s15(0.8249)))
    tags.append((b"rXYZ", b"XYZ \0\0\0\0" + s15(0.4361) + s15(0.2225) + s15(0.0139)))
    tags.append((b"gXYZ", b"XYZ \0\0\0\0" + s15(0.3851) + s15(0.7169) + s15(0.0971)))
    tags.append((b"bXYZ", b"XYZ \0\0\0\0" + s15(0.1431) + s15(0.0606) + s15(0.7141)))
    curve = [int(65535 * (i / 15.0) ** 1.8 + 0.5) for i in range(16)]
    curv = b"curv\0\0\0\0" + struct.pack(">I", len(curve)) + b"".join(struct.pack(">H", v) for v in curve)
    for name in (b"rTRC", b"gTRC", b"bTRC"):
        tags.append((name, curv))
    offset = 128 + 4 + 12 * len(tags)
    table = struct.pack(">I", len(tags))
    body = b""
    for name, data in tags:
        while (offset + len(body)) % 4:
            body += b"\0"
        table += name + struct.pack(">II", offset + len(body), len(data))
        body += data
    size = 128 + len(table) + len(body)
    header = struct.pack(">I", size) + b"none" + bytes([2, 0x10, 0, 0]) + b"mntr" + b"RGB " + b"XYZ "
    header += b"\0" * 12 + b"acsp" + b"\0" * 4 + b"\0" * 4 + b"\0" * 4 + b"\0" * 4 + b"\0" * 8 + b"\0" * 4
    header += s15(0.9642) + s15(1.0) + s15(0.8249) + b"\0" * 4 + b"\0" * 16 + b"\0" * 28
    assert len(header) == 128, len(header)
    icc = header + table + body
    rnd = Lcg(7)
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            row += rgb_pixel(x, y, rnd)
        rows.append(bytes(row))
    png("src_icc.png", W, H, 2, 8, rows, chunk(b"iCCP", b"test\0\0" + zlib.compress(icc, 9)))


make_rgb()
make_rgba()
make_grey()
make_rgb16()
make_pfm()
make_exif_png()
make_orient_png()
make_anim()
make_icc_png()
