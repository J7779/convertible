# Regenerates the JPEG fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow, ImageMagick 7 (`magick`, built with libjpeg-turbo) and macOS `sips`.
# Content is synthetic.
import struct
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def gradient(w, h):
    # Same pattern as cvt::test::gradient_frame.
    img = Image.new("RGB", (w, h))
    img.putdata([(ramp(x, w), ramp(y, h), 255 - ramp(x + y, w + h - 1)) for y in range(h) for x in range(w)])
    return img


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


def magick(*args):
    *inputs, output = args
    run("magick", *inputs, "-strip", output)


W, H = 45, 29  # deliberately not a multiple of the MCU size
gradient(W, H).save("source.ppm")

magick("source.ppm", "-quality", "90", "-sampling-factor", "1x1", "q90_444.jpg")
magick("source.ppm", "-quality", "75", "-sampling-factor", "2x2", "q75_420.jpg")
magick("source.ppm", "-quality", "85", "-sampling-factor", "2x1", "q85_422.jpg")
magick("source.ppm", "-quality", "80", "-sampling-factor", "4x1", "q80_411.jpg")
magick("source.ppm", "-quality", "80", "-sampling-factor", "1x2", "q80_440.jpg")
magick("source.ppm", "-quality", "90", "-colorspace", "Gray", "grey.jpg")
magick("source.ppm", "-quality", "80", "-sampling-factor", "2x2", "-interlace", "JPEG", "progressive.jpg")
magick("source.ppm", "-quality", "80", "-define", "jpeg:restart-interval=1", "restart.jpg")
magick("source.ppm", "-quality", "92", "-colorspace", "CMYK", "cmyk.jpg")
magick("source.ppm", "-quality", "85", "-define", "jpeg:arithmetic-coding=true", "arith.jpg")
magick("source.ppm", "-quality", "85", "-interlace", "JPEG", "-define", "jpeg:arithmetic-coding=true",
       "arith_progressive.jpg")

# Reference decodes by libjpeg-turbo (through ImageMagick) for exact comparisons.
for name in ("q90_444", "q75_420", "q85_422", "progressive", "cmyk", "q80_411"):
    run("magick", f"{name}.jpg", "-colorspace", "sRGB", "-strip", f"{name}.ppm")

# EXIF orientation 6 (rotate 90 degrees clockwise to display): a 16x8 picture
# whose stored top-left 4x4 corner is red, with a minimal EXIF APP1 segment.
marker = Image.new("RGB", (16, 8), (0, 0, 255))
for y in range(4):
    for x in range(4):
        marker.putpixel((x, y), (255, 0, 0))
marker.save("orient_tmp.ppm")
magick("orient_tmp.ppm", "-quality", "95", "-sampling-factor", "1x1", "orient_tmp.jpg")
tiff = b"MM\x00\x2a" + struct.pack(">I", 8) + struct.pack(">H", 1) + struct.pack(">HHIHH", 0x0112, 3, 1, 6, 0)
tiff += struct.pack(">I", 0)
app1 = b"\xff\xe1" + struct.pack(">H", len(tiff) + 8) + b"Exif\x00\x00" + tiff
data = open("orient_tmp.jpg", "rb").read()
open("orientation6.jpg", "wb").write(data[:2] + app1 + data[2:])
run("rm", "orient_tmp.ppm", "orient_tmp.jpg")

# Pillow: optimised Huffman tables, an ICC profile and a comment.
icc = b"\x00" * 128  # a placeholder profile body; the reader only passes it through
gradient(W, H).save("pillow_icc.jpg", quality=88, optimize=True, icc_profile=icc, comment=b"synthetic test pattern")

# Apple ImageIO.
run("sips", "-s", "format", "jpeg", "-s", "formatOptions", "80", "source.ppm", "--out", "sips.jpg")

# MPO: two JPEGs, the first carrying an APP2 "MPF" segment (CIPA DC-007).
first = open("q90_444.jpg", "rb").read()
second = open("grey.jpg", "rb").read()
# MP header: endian, offset to first IFD, IFD with MPFVersion, NumberOfImages, MPEntry.
mp_entries = 2
ifd = struct.pack(">H", 3)
ifd += struct.pack(">HHI4s", 0xB000, 7, 4, b"0100")
ifd += struct.pack(">HHII", 0xB001, 4, 1, mp_entries)
entry_offset = 8 + 2 + 3 * 12 + 4
ifd += struct.pack(">HHII", 0xB002, 7, 16 * mp_entries, entry_offset)
ifd += struct.pack(">I", 0)
mpf_body_len = 8 + len(ifd) - 8 + 16 * mp_entries
app2_len = 2 + 4 + 8 + (len(ifd)) + 16 * mp_entries
total_first = len(first) + app2_len + 2
entries = struct.pack(">IIIHH", 0x20030000, total_first, 0, 0, 0)
mpf_offset_base = 2 + 2 + 2 + 4  # SOI + marker + length + "MPF\0" -> start of the TIFF header
entries += struct.pack(">IIIHH", 0x00000000, len(second), total_first - mpf_offset_base, 0, 0)
app2 = b"\xff\xe2" + struct.pack(">H", app2_len) + b"MPF\x00" + b"MM\x00\x2a" + struct.pack(">I", 8) + ifd + entries
open("stereo.mpo", "wb").write(first[:2] + app2 + first[2:] + second)
