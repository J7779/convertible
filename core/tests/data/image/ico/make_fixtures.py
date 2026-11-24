# Regenerates the ICO, CUR and ICNS fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow, ImageMagick 7 (`magick`) and macOS `iconutil`. Content is synthetic.
import os
import struct
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def alpha_gradient(n):
    img = Image.new("RGBA", (n, n))
    img.putdata([(ramp(x, n), ramp(y, n), 255 - ramp(x + y, 2 * n - 1), ramp(x + y, 2 * n - 1))
                 for y in range(n) for x in range(n)])
    return img


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


alpha_gradient(48).save("a48.png")
# Multi-size icon: 48 (the source itself), 32 and 16 as 32-bit bitmaps.
run("magick", "a48.png", "-define", "icon:auto-resize=48,32,16", "multi.ico")
# A PNG-compressed entry (Pillow writes the PNG; the directory is written by hand).
import io
buf = io.BytesIO()
alpha_gradient(64).save(buf, format="PNG")
png = buf.getvalue()
open("png_entry.ico", "wb").write(struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 64, 64, 0, 0, 1, 32, len(png), 22) + png)
# Cursor.
run("magick", "a48.png", "CUR:cursor.cur")

# 8-bit palette entry with an AND mask, written by hand from the format
# description: 16x16, palette entry i = (i, 255 - i, 128), pixel (x, y) uses
# entry (x + 16 * y) % 256, the left 4 columns are masked out (transparent).
n = 16
header = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 8, 0, 0, 0, 0, 256, 0)
palette = b"".join(struct.pack("<BBBB", 128, 255 - i, i, 0) for i in range(256))
pixels = b"".join(bytes(((x + n * y) % 256) for x in range(n)) for y in reversed(range(n)))
mask_row = bytes([0xF0, 0x00, 0x00, 0x00])
mask = mask_row * n
dib = header + palette + pixels + mask
entry = struct.pack("<BBBBHHII", n, n, 0, 0, 1, 8, len(dib), 6 + 16)
open("pal8_mask.ico", "wb").write(struct.pack("<HHH", 0, 1, 1) + entry + dib)

# ICNS from an iconset (Apple iconutil).
os.makedirs("t.iconset", exist_ok=True)
for size, name in ((16, "icon_16x16.png"), (32, "icon_16x16@2x.png"), (32, "icon_32x32.png"),
                   (64, "icon_32x32@2x.png"), (128, "icon_128x128.png")):
    alpha_gradient(size).save(os.path.join("t.iconset", name))
run("iconutil", "-c", "icns", "t.iconset", "-o", "iconset.icns")

# ICNS with run-length coded RGB (il32) and an 8-bit mask (l8mk), by hand:
# 32x32, R = x * 8, G = y * 8, B = 77; mask = 255 except the top row (0).
def rle_channel(values):
    out = bytearray()
    i = 0
    while i < len(values):
        run_len = 1
        while i + run_len < len(values) and run_len < 130 and values[i + run_len] == values[i]:
            run_len += 1
        if run_len >= 3:
            out += bytes([run_len + 125, values[i]])
            i += run_len
        else:
            lit = values[i:i + min(128, len(values) - i)]
            take = 1
            while take < len(lit) and not (take + 2 < len(lit) and lit[take] == lit[take + 1] == lit[take + 2]):
                take += 1
            out += bytes([take - 1]) + bytes(lit[:take])
            i += take
    return bytes(out)


size = 32
r = [x * 8 for y in range(size) for x in range(size)]
g = [y * 8 for y in range(size) for x in range(size)]
b = [77] * (size * size)
il32 = rle_channel(r) + rle_channel(g) + rle_channel(b)
l8mk = bytes(0 if y == 0 else 255 for y in range(size) for x in range(size))
body = b"il32" + struct.pack(">I", len(il32) + 8) + il32 + b"l8mk" + struct.pack(">I", len(l8mk) + 8) + l8mk
open("rle.icns", "wb").write(b"icns" + struct.pack(">I", len(body) + 8) + body)

for name in os.listdir("t.iconset"):
    os.remove(os.path.join("t.iconset", name))
os.rmdir("t.iconset")
os.remove("a48.png")
