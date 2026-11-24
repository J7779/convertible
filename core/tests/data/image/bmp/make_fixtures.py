# Regenerates the BMP fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow, ImageMagick 7 (`magick`) and macOS `sips`. Content is synthetic.
import struct
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


W, H = 32, 24
grad = Image.new("RGB", (W, H))
grad.putdata([(ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1)) for y in range(H) for x in range(W)])
grad.save("grad_tmp.png")
alpha = Image.new("RGBA", (W, H))
alpha.putdata([(ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1), ramp(x + y, W + H - 1))
               for y in range(H) for x in range(W)])
alpha.save("alpha_tmp.png")
pal = Image.new("P", (W, H))
pal.putpalette([v for i in range(256) for v in ((i * 7) % 256, (i * 13) % 256, (i * 29) % 256)])
pal.putdata([(x * 5 + y * 3) % 200 for y in range(H) for x in range(W)])
pal.save("pal_tmp.png")
pal16 = Image.new("P", (W, H))
pal16.putpalette([v for i in range(16) for v in ((i * 70) % 256, (i * 130) % 256, (255 - i * 50) % 256)])
pal16.putdata([((x // 3) + y) % 16 for y in range(H) for x in range(W)])
pal16.save("pal16_tmp.png")

run("magick", "grad_tmp.png", "BMP3:rgb24.bmp")
run("magick", "alpha_tmp.png", "BMP:rgba32_v5.bmp")
run("magick", "pal_tmp.png", "-type", "Palette", "-compress", "None", "BMP3:pal8.bmp")
run("magick", "pal16_tmp.png", "-type", "Palette", "-compress", "None", "BMP3:pal4.bmp")
run("magick", "pal_tmp.png", "-type", "Palette", "-compress", "RLE", "BMP3:rle8.bmp")
run("magick", "grad_tmp.png", "-define", "bmp:subtype=RGB565", "BMP:rgb565.bmp")
run("magick", "alpha_tmp.png", "-define", "bmp:subtype=ARGB1555", "BMP:argb1555.bmp")
run("magick", "grad_tmp.png", "BMP2:os2.bmp")
run("sips", "-s", "format", "bmp", "grad_tmp.png", "--out", "sips.bmp")

# 1-bit checkerboard (Pillow).
bw = Image.new("1", (W, H))
bw.putdata([255 if ((x // 4) + (y // 4)) % 2 else 0 for y in range(H) for x in range(W)])
bw.save("mono1.bmp")

# Top-down variant of rgb24.bmp: negative height, rows stored top to bottom.
data = bytearray(open("rgb24.bmp", "rb").read())
offset = struct.unpack_from("<I", data, 10)[0]
stride = (W * 3 + 3) & ~3
rows = [data[offset + r * stride: offset + (r + 1) * stride] for r in range(H)]
data[offset:offset + stride * H] = b"".join(reversed(rows))
struct.pack_into("<i", data, 22, -H)
open("topdown.bmp", "wb").write(bytes(data))

# RLE4 written by hand from the specification (ImageMagick has no RLE4 writer):
# 8x4 pixels, 16-colour palette (entry i = (i*16, 255-i*16, 0)). Rows (top to
# bottom): 0 1 0 1 0 1 0 1 / 2 2 2 2 3 4 5 6 / 7 8 9 10 11 12 13 14 / 15 x 8
# where the last row is skipped with a delta and stays transparent.
pal_bytes = b"".join(struct.pack("<BBBB", 0, 255 - i * 16, i * 16, 0) for i in range(16))
rle = bytearray()
rle += bytes([0, 2, 0, 1])                       # delta: skip the bottom row (file rows are bottom-up)
rle += bytes([0, 8, 0x78, 0x9A, 0xBC, 0xDE])     # absolute 8 pixels: 7..14
rle += bytes([0, 0])                             # end of line
rle += bytes([4, 0x22, 0, 4, 0x34, 0x56])        # run of 4 x 2, absolute 3 4 5 6
rle += bytes([0, 0])
rle += bytes([8, 0x01])                          # alternating run 0 1 0 1 ...
rle += bytes([0, 0, 0, 1])                       # end of line, end of bitmap
header = struct.pack("<IiiHHIIiiII", 40, 8, 4, 1, 4, 2, len(rle), 2835, 2835, 16, 0)
offset = 14 + len(header) + len(pal_bytes)
open("rle4.bmp", "wb").write(b"BM" + struct.pack("<IHHI", offset + len(rle), 0, 0, offset) + header + pal_bytes + rle)

for name in ("grad_tmp.png", "alpha_tmp.png", "pal_tmp.png", "pal16_tmp.png"):
    run("rm", name)
