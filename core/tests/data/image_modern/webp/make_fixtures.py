#!/usr/bin/env python3
"""Regenerate the WebP fixtures of this directory.

Synthetic test patterns are written with the Python standard library; every
.webp file is produced by Google's libwebp command line tools (cwebp, dwebp,
img2webp, gif2webp, webpmux, anim_dump, release 1.6.0) or by ImageMagick 7.

    WEBP_TOOLS=<directory holding the libwebp tools> python3 make_fixtures.py

Run it from this directory; it only reads and writes files here and in a
temporary "work" subdirectory that it deletes at the end.
"""
import os
import shutil
import struct
import subprocess

TOOLS = os.environ.get("WEBP_TOOLS", "")
WORK = "work"


def tool(name):
    return os.path.join(TOOLS, name) if TOOLS else name


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def write_ppm(path, width, height, pixel):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (width, height))
        for y in range(height):
            for x in range(width):
                f.write(bytes(pixel(x, y)[:3]))


def write_pam(path, width, height, pixel):
    with open(path, "wb") as f:
        f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (width, height))
        for y in range(height):
            for x in range(width):
                f.write(bytes(pixel(x, y)))


def pattern_rgb(x, y):
    # Smooth ramps, a checker band and a hard diagonal edge.
    r = (x * 4) & 255
    g = (y * 5) & 255
    b = ((x ^ y) * 8) & 255
    if 16 <= y < 24:
        r = g = b = 255 if ((x // 4) + (y // 4)) % 2 else 0
    if x > y + 30:
        r, g, b = 200, 40, 90
    return (r, g, b, 255)


def pattern_rgba(x, y):
    r, g, b, _ = pattern_rgb(x, y)
    a = min(255, x * 5 + y * 2)
    if y >= 40:
        a = 0  # fully transparent rows with colour underneath (kept by -exact)
    if x < 8:
        a = 255
    return (r, g, b, a)


def photo_pixel(x, y):
    # Smooth shading plus deterministic grain, like a small photograph.
    n = (x * 73856093 ^ y * 19349663) & 0xFFFFFFFF
    n = (n * 1103515245 + 12345) & 0xFFFFFFFF
    grain = ((n >> 16) % 13) - 6
    r = 60 + x * 2 + grain
    g = 40 + y * 2 + (x * y) // 64 + grain
    b = 150 - x + y + grain
    return tuple(max(0, min(255, v)) for v in (r, g, b)) + (255,)


def frame_pixel(background, square, sx, sy, size=8):
    def pixel(x, y):
        if sx <= x < sx + size and sy <= y < sy + size:
            return square
        return background
    return pixel


def exif_orientation_6():
    # Little-endian TIFF: IFD0 with Orientation = 6 and Software = "pattern".
    entries = [(0x0112, 3, 1, struct.pack("<HH", 6, 0)), (0x0131, 2, 8, None)]
    header = b"II*\x00" + struct.pack("<I", 8)
    ifd_size = 2 + 12 * len(entries) + 4
    text_offset = 8 + ifd_size
    ifd = struct.pack("<H", len(entries))
    for tag, kind, count, value in entries:
        if value is None:
            value = struct.pack("<I", text_offset)
        ifd += struct.pack("<HHI", tag, kind, count) + value
    ifd += struct.pack("<I", 0)
    return header + ifd + b"pattern\x00"


def fake_icc():
    # Not a real profile: a 132-byte header-shaped blob, stored verbatim.
    body = bytearray(132)
    struct.pack_into(">I", body, 0, len(body))
    body[4:8] = b"none"
    body[12:16] = b"mntr"
    body[16:20] = b"RGB "
    body[20:24] = b"XYZ "
    body[36:40] = b"acsp"
    return bytes(body)


XMP = (b'<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>'
       b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
       b'<rdf:Description xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>pattern</dc:title>'
       b'</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>')


def main():
    os.makedirs(WORK, exist_ok=True)
    w = lambda name: os.path.join(WORK, name)

    # References kept in the repository.
    write_ppm("src_rgb.ppm", 64, 48, pattern_rgb)
    write_pam("src_rgba.pam", 64, 48, pattern_rgba)

    # Stills.
    run(tool("cwebp"), "-quiet", "-lossless", "src_rgb.ppm", "-o", "lossless.webp")
    run(tool("cwebp"), "-quiet", "-lossless", "-exact", "src_rgba.pam", "-o", "lossless_alpha.webp")
    run(tool("cwebp"), "-quiet", "-q", "80", "src_rgb.ppm", "-o", "lossy.webp")
    run(tool("dwebp"), "-quiet", "lossy.webp", "-ppm", "-o", "lossy_dwebp.ppm")
    run(tool("cwebp"), "-quiet", "-q", "80", "-alpha_q", "90", "src_rgba.pam", "-o", "lossy_alpha.webp")
    run(tool("dwebp"), "-quiet", "lossy_alpha.webp", "-pam", "-o", "lossy_alpha_dwebp.pam")
    write_ppm(w("photo.ppm"), 96, 64, photo_pixel)
    run(tool("cwebp"), "-quiet", "-lossless", w("photo.ppm"), "-o", "photo_lossless.webp")
    run("magick", "src_rgb.ppm", "-strip", "-quality", "60", "magick_lossy.webp")
    run(tool("dwebp"), "-quiet", "magick_lossy.webp", "-ppm", "-o", "magick_lossy_dwebp.ppm")
    run("magick", "src_rgba.pam", "-strip", "-define", "webp:lossless=true", "-define", "webp:exact=true",
        "magick_lossless_alpha.webp")

    # Metadata and EXIF orientation: a 16x8 asymmetric pattern, rotated by EXIF.
    write_pam(w("small.pam"), 16, 8, lambda x, y: (x * 16, y * 32, 128 if x < 4 else 0, 255))
    run(tool("cwebp"), "-quiet", "-lossless", w("small.pam"), "-o", w("small.webp"))
    with open(w("exif.bin"), "wb") as f:
        f.write(exif_orientation_6())
    with open(w("profile.icc"), "wb") as f:
        f.write(fake_icc())
    with open(w("meta.xmp"), "wb") as f:
        f.write(XMP)
    run(tool("webpmux"), "-set", "exif", w("exif.bin"), w("small.webp"), "-o", w("m1.webp"))
    run(tool("webpmux"), "-set", "icc", w("profile.icc"), w("m1.webp"), "-o", w("m2.webp"))
    run(tool("webpmux"), "-set", "xmp", w("meta.xmp"), w("m2.webp"), "-o", "metadata.webp")
    run("magick", w("small.pam"), "-rotate", "90", "metadata_upright.pam")

    # Animation assembled frame by frame: offsets, dispose and blend methods.
    frames = [
        ((200, 30, 30, 255), (30, 30, 200, 255), 0, 0, 32, 24),   # full canvas, red with a blue square
        ((0, 0, 0, 0), (30, 200, 30, 255), 0, 0, 8, 8),           # 8x8 green, blended
        ((0, 0, 0, 0), (250, 250, 20, 128), 0, 0, 8, 8),          # 8x8 half-transparent yellow
        ((0, 0, 0, 0), (255, 255, 255, 255), 0, 0, 4, 4),         # 4x4 white
    ]
    for i, (bg, sq, sx, sy, fw, fh) in enumerate(frames):
        pixel = frame_pixel(bg, sq, 8, 8) if i == 0 else (lambda x, y, c=sq: c)
        write_pam(w("f%d.pam" % i), fw, fh, pixel)
        run(tool("cwebp"), "-quiet", "-lossless", "-exact", w("f%d.pam" % i), "-o", w("f%d.webp" % i))
    run(tool("webpmux"),
        "-frame", w("f0.webp"), "+100+0+0+0-b",
        "-frame", w("f1.webp"), "+200+8+4+1+b",      # disposed to background afterwards
        "-frame", w("f2.webp"), "+300+16+8+0+b",     # alpha-blended onto the canvas
        "-frame", w("f3.webp"), "+400+2+2+0-b",      # replaces (no blend)
        "-loop", "3", "-bgcolor", "255,255,255,255", "-o", "anim_mux.webp")
    run(tool("anim_dump"), "-folder", ".", "-prefix", "anim_mux_", "-pam", "anim_mux.webp")

    # Lossy animation from img2webp (sub-frames chosen by the animation encoder).
    for i in range(3):
        write_pam(w("m%d.pam" % i), 32, 24, frame_pixel((40, 90, 160, 255), (240, 200, 40, 255), 4 + 8 * i, 6))
    run(tool("img2webp"), "-loop", "0", "-lossy", "-q", "70",
        "-d", "80", w("m0.pam"), "-d", "120", w("m1.pam"), "-d", "40", w("m2.pam"), "-o", "anim_lossy.webp")
    run(tool("anim_dump"), "-folder", ".", "-prefix", "anim_lossy_", "-pam", "anim_lossy.webp")

    # GIF animation (ImageMagick) converted by gif2webp.
    run("magick", "-size", "24x16", "-delay", "10", "xc:red", "-delay", "20", "xc:lime", "-delay", "30",
        "xc:blue", "-loop", "2", w("rgb.gif"))
    run(tool("gif2webp"), "-quiet", w("rgb.gif"), "-o", "anim_gif2webp.webp")

    shutil.rmtree(WORK)


if __name__ == "__main__":
    main()
