# Regenerates the PNG fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow and ImageMagick 7 (`magick`) on PATH. Content is synthetic.
import io
import struct
import subprocess
import zlib

from PIL import Image, PngImagePlugin


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def gradient(w, h):
    # Same pattern as cvt::test::gradient_frame.
    img = Image.new("RGB", (w, h))
    img.putdata([(ramp(x, w), ramp(y, h), 255 - ramp(x + y, w + h - 1)) for y in range(h) for x in range(w)])
    return img


def alpha_gradient(w, h):
    # Same pattern as cvt::test::alpha_gradient_frame.
    img = Image.new("RGBA", (w, h))
    img.putdata([(ramp(x, w), ramp(y, h), 255 - ramp(x + y, w + h - 1), ramp(x + y, w + h - 1))
                 for y in range(h) for x in range(w)])
    return img


def wide_ppm(path, w, h, channels):
    # 16-bit samples: value(x, y, c) = (x * 2039 + y * 977 + c * 12345) % 65536
    maxval = 65535
    tupl = {1: "GRAYSCALE", 2: "GRAYSCALE_ALPHA", 3: "RGB", 4: "RGB_ALPHA"}[channels]
    header = f"P7\nWIDTH {w}\nHEIGHT {h}\nDEPTH {channels}\nMAXVAL {maxval}\nTUPLTYPE {tupl}\nENDHDR\n".encode()
    body = bytearray()
    for y in range(h):
        for x in range(w):
            for c in range(channels):
                v = (x * 2039 + y * 977 + c * 12345) % 65536
                if c == channels - 1 and channels in (2, 4):
                    v = 65535 if (x + y) % 3 else (x * 4096) % 65536
                body += struct.pack(">H", v)
    with open(path, "wb") as f:
        f.write(header + body)


def magick(*args):
    # -strip and the exclusions keep timestamps and tool comments out of the files.
    *inputs, output = args
    subprocess.run(["magick", *inputs, "-strip", "-define", "png:exclude-chunks=date,time,tEXt,zTXt,bKGD", output],
                   check=True)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def insert_after_ihdr(png, extra):
    return png[:33] + extra + png[33:]


W, H = 32, 24
gradient(W, H).save("rgb8.png")
alpha_gradient(W, H).save("rgba8.png")
magick("rgb8.png", "-interlace", "PNG", "PNG24:rgb8_interlaced.png")
magick("rgba8.png", "-interlace", "PNG", "PNG32:rgba8_interlaced.png")

# Grey ramps at 1, 2, 4, 8 bits: level = ramp(x, W) >> (8 - depth), replicated to 8 bits.
for depth in (1, 2, 4, 8):
    levels = (1 << depth) - 1
    img = Image.new("L", (W, H))
    img.putdata([(ramp(x, W) >> (8 - depth)) * 255 // levels for y in range(H) for x in range(W)])
    img.save("grey_tmp.png")
    magick("grey_tmp.png", "-depth", str(depth), "-define", "png:color-type=0", "-define",
           f"png:bit-depth={depth}", f"grey{depth}.png")
subprocess.run(["rm", "grey_tmp.png"], check=True)

# 16-bit variants from PAM sources.
for channels, name, ctype in ((1, "grey16", 0), (2, "greyalpha16", 4), (3, "rgb16", 2), (4, "rgba16", 6)):
    wide_ppm("tmp.pam", 16, 12, channels)
    magick("tmp.pam", "-define", f"png:color-type={ctype}", "-define", "png:bit-depth=16", f"{name}.png")
magick("tmp.pam", "-interlace", "PNG", "-define", "png:color-type=6", "-define", "png:bit-depth=16",
       "rgba16_interlaced.png")
subprocess.run(["rm", "tmp.pam"], check=True)

# Grey + alpha, 8 bit.
la = alpha_gradient(W, H).convert("LA")
la.save("greyalpha8.png")

# Palettes: 200 colours (8 bit), 16 colours (4 bit), 4 colours (2 bit), 2 colours (1 bit).
pal = Image.new("P", (W, H))
pal.putpalette([v for i in range(256) for v in ((i * 7) % 256, (i * 13) % 256, (i * 29) % 256)])
pal.putdata([(x * 5 + y * 3) % 200 for y in range(H) for x in range(W)])
pal.save("palette8.png")
for depth, count in ((4, 16), (2, 4), (1, 2)):
    p = Image.new("P", (W, H))
    p.putpalette([v for i in range(count) for v in ((i * 70) % 256, (i * 130) % 256, (255 - i * 50) % 256)])
    p.putdata([(x + y) % count for y in range(H) for x in range(W)])
    p.save(f"palette{depth}.png", bits=depth)

# Palette with per-entry alpha (tRNS).
pt = Image.new("P", (W, H))
pt.putpalette([v for i in range(8) for v in (i * 30, 255 - i * 30, 128)])
pt.putdata([(x // 4 + y // 4) % 8 for y in range(H) for x in range(W)])
pt.save("palette_trns.png", transparency=bytes([0, 64, 128, 192, 255, 255, 255, 255]))

# Colour-key transparency for RGB and grey.
key_rgb = gradient(W, H)
key_rgb.putpixel((0, 0), (1, 2, 3))
key_rgb.save("rgb_trns.png", transparency=(1, 2, 3))
key_grey = Image.new("L", (W, H))
key_grey.putdata([(x * 8) % 256 for y in range(H) for x in range(W)])
key_grey.save("grey_trns.png", transparency=0)

# gAMA 1.0: the stored values are linear light.
lin = Image.new("RGB", (16, 1))
lin.putdata([(i * 17, i * 17, i * 17) for i in range(16)])
buf = io.BytesIO()
lin.save(buf, format="PNG")
with open("gamma10.png", "wb") as f:
    f.write(insert_after_ihdr(buf.getvalue(), chunk(b"gAMA", struct.pack(">I", 100000))))

# Metadata: text chunks, pHYs, eXIf with orientation 6 (rotate 90 CW), iCCP.
meta = Image.new("RGB", (4, 2), (0, 0, 0))
meta.putpixel((0, 0), (255, 0, 0))  # red marker at the stored top-left
info = PngImagePlugin.PngInfo()
info.add_text("Title", "Test pattern")
info.add_text("Comment", "compressed text", zip=True)
info.add_itxt("Description", "café ✓", lang="en", tkey="Description")
exif = Image.Exif()
exif[0x0112] = 6
meta.save("metadata.png", pnginfo=info, dpi=(150, 150), exif=exif.tobytes())

# APNG (Pillow): 3 frames, partial regions, disposal and blending modes, loop twice.
frames = []
for n in range(3):
    f = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for y in range(H):
        for x in range(W):
            if x < 8 * (n + 1):
                f.putpixel((x, y), (80 * n, 255 - 80 * n, 40, 255))
    frames.append(f)
frames[0].save("anim.png", save_all=True, append_images=frames[1:], duration=[100, 200, 300], loop=2,
               disposal=[0, 0, 0], blend=[0, 1, 0])

# Apple ImageIO (sips) writes the gradient as well.
magick("rgb8.png", "BMP3:sips_tmp.bmp")
subprocess.run(["sips", "-s", "format", "png", "sips_tmp.bmp", "--out", "sips_rgb8.png"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["rm", "sips_tmp.bmp"], check=True)
