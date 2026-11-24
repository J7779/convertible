# Regenerates the TIFF fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow (with libtiff) and ImageMagick 7 (`magick`, libtiff 4.7). Content is synthetic.
import struct
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


def magick(*args):
    *inputs, output = args
    run("magick", *inputs, "-strip", "-define", "tiff:document=", output)


W, H = 32, 24
grad = Image.new("RGB", (W, H))
grad.putdata([(ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1)) for y in range(H) for x in range(W)])
grad.save("grad.ppm")
alpha = Image.new("RGBA", (W, H))
alpha.putdata([(ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1), ramp(x + y, W + H - 1))
               for y in range(H) for x in range(W)])
alpha.save("alpha_tmp.png")

# 16-bit source: value(x, y, c) = (x * 2039 + y * 977 + c * 12345) % 65536
wide = bytearray()
for y in range(H):
    for x in range(W):
        for c in range(3):
            wide += struct.pack(">H", (x * 2039 + y * 977 + c * 12345) % 65536)
open("wide_tmp.ppm", "wb").write(b"P6\n%d %d\n65535\n" % (W, H) + wide)

magick("grad.ppm", "-compress", "None", "rgb8_none.tif")
magick("grad.ppm", "-compress", "LZW", "-define", "tiff:predictor=2", "rgb8_lzw_predictor.tif")
magick("grad.ppm", "-compress", "RLE", "rgb8_packbits.tif")
magick("grad.ppm", "-compress", "LZW", "-endian", "MSB", "rgb8_lzw_bigendian.tif")
magick("grad.ppm", "-compress", "Zip", "-define", "tiff:tile-geometry=16x16", "rgb8_tiled_deflate.tif")
magick("grad.ppm", "-compress", "None", "-interlace", "Plane", "rgb8_planar.tif")
magick("grad.ppm", "-compress", "LZW", "TIFF64:rgb8_bigtiff.tif")
magick("grad.ppm", "-compress", "JPEG", "-quality", "90", "jpeg.tif")
magick("grad.ppm", "-colorspace", "CMYK", "-compress", "LZW", "cmyk.tif")
magick("grad.ppm", "-colorspace", "Lab", "-compress", "None", "lab.tif")
magick("grad.ppm", "-orient", "RightTop", "-compress", "None", "orientation6.tif")
magick("alpha_tmp.png", "-compress", "Zip", "rgba8_deflate.tif")
magick("wide_tmp.ppm", "-compress", "LZW", "-define", "tiff:predictor=2", "rgb16_lzw.tif")
magick("wide_tmp.ppm", "-colorspace", "Gray", "-compress", "Zip", "grey16_deflate.tif")
magick("wide_tmp.ppm", "-define", "quantum:format=floating-point", "-depth", "32", "-compress", "Zip",
       "-define", "tiff:predictor=3", "float32_predictor3.tif")
magick("wide_tmp.ppm", "-define", "quantum:format=floating-point", "-depth", "16", "-compress", "None",
       "float16.tif")

# Palette.
pal = Image.new("P", (W, H))
pal.putpalette([v for i in range(256) for v in ((i * 7) % 256, (i * 13) % 256, (i * 29) % 256)])
pal.putdata([(x * 5 + y * 3) % 200 for y in range(H) for x in range(W)])
pal.save("palette_tmp.png")
magick("palette_tmp.png", "-type", "Palette", "-compress", "LZW", "palette8.tif")

# Bilevel: a 4-pixel checkerboard with a diagonal, in every fax compression.
bw = Image.new("1", (61, 23))
bw.putdata([255 if (((x // 4) + (y // 4)) % 2 == 0) != (x == y) else 0 for y in range(23) for x in range(61)])
bw.save("mono_g4.tif", compression="group4")
bw.save("mono_g3.tif", compression="group3")
bw.save("mono_mh.tif", compression="tiff_ccitt")
bw.save("mono_packbits.tif", compression="packbits")
bw.save("bw_tmp.png")
magick("bw_tmp.png", "-compress", "Fax", "-define", "tiff:fill-order=lsb", "mono_g3_lsb.tif")

# Multi-page (three pages of the same size).
magick("grad.ppm", "(", "grad.ppm", "-negate", ")", "(", "grad.ppm", "-flop", ")", "-compress", "LZW",
       "multipage.tif")

# Associated (premultiplied) alpha, written by hand from the specification
# (ImageMagick stores straight colour under that tag): uncompressed RGBA with
# ExtraSamples = 1, colour = round(c * a / 255).
pixels = bytearray()
for y in range(H):
    for x in range(W):
        a = ramp(x + y, W + H - 1)
        for c in (ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1)):
            pixels.append((c * a + 127) // 255)
        pixels.append(a)
entries = [(256, 3, 1, W), (257, 3, 1, H), (258, 3, 4, 0), (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, 0),
           (277, 3, 1, 4), (278, 3, 1, H), (279, 4, 1, len(pixels)), (284, 3, 1, 1), (338, 3, 1, 1)]
ifd_offset = 8
ifd_size = 2 + 12 * len(entries) + 4
bits_offset = ifd_offset + ifd_size
data_offset = bits_offset + 8
out = bytearray(b"II*\x00" + struct.pack("<I", ifd_offset))
out += struct.pack("<H", len(entries))
for tag, typ, count, value in entries:
    if tag == 258:
        value = bits_offset
    if tag == 273:
        value = data_offset
    if typ == 3 and count == 1:
        out += struct.pack("<HHIHH", tag, typ, count, value, 0)
    else:
        out += struct.pack("<HHII", tag, typ, count, value)
out += struct.pack("<I", 0)
out += struct.pack("<HHHH", 8, 8, 8, 8)
out += pixels
open("rgba8_associated.tif", "wb").write(bytes(out))

for name in ("alpha_tmp.png", "wide_tmp.ppm", "palette_tmp.png", "bw_tmp.png", "grad.ppm"):
    run("rm", name)
