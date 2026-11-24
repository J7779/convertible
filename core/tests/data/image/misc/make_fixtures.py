# Regenerates the fixtures of the smaller raster formats (see SOURCES.md):
# Netpbm (PBM/PGM/PPM/PAM/PFM), QOI, TGA, PCX, Radiance HDR, farbfeld, WBMP,
# XBM, XPM, SGI and Sun raster.
# Run from this directory: python3 make_fixtures.py
# Needs Pillow and ImageMagick 7 (`magick`). Content is synthetic.
import struct
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


def magick(*args):
    *inputs, output = args
    run("magick", *inputs, "-strip", output)


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
pal.convert("RGB").save("pal_tmp.png")
checker = Image.new("L", (W, H))
checker.putdata([0 if ((x // 4) + (y // 4)) % 2 == 0 else 255 for y in range(H) for x in range(W)])
checker.save("checker_tmp.png")
# 16-bit source: value(x, y, c) = (x * 2039 + y * 977 + c * 12345) % 65536
wide = bytearray()
for y in range(H):
    for x in range(W):
        for c in range(3):
            wide += struct.pack(">H", (x * 2039 + y * 977 + c * 12345) % 65536)
open("wide_tmp.ppm", "wb").write(b"P6\n%d %d\n65535\n" % (W, H) + wide)

# Netpbm
magick("grad_tmp.png", "PPM:grad.ppm")
magick("grad_tmp.png", "-compress", "none", "PPM:grad_ascii.ppm")
magick("wide_tmp.ppm", "-colorspace", "Gray", "PGM:grey16.pgm")
magick("checker_tmp.png", "PBM:checker.pbm")
magick("checker_tmp.png", "-compress", "none", "PBM:checker_ascii.pbm")
magick("alpha_tmp.png", "PAM:alpha.pam")
magick("wide_tmp.ppm", "-colorspace", "RGB", "-endian", "LSB", "PFM:wide.pfm")
# QOI, TGA, PCX
magick("alpha_tmp.png", "QOI:alpha.qoi")
magick("grad_tmp.png", "QOI:grad.qoi")
magick("grad_tmp.png", "-compress", "RLE", "TGA:grad_rle.tga")
magick("alpha_tmp.png", "-compress", "None", "TGA:alpha.tga")
magick("pal_tmp.png", "-type", "Palette", "-compress", "None", "TGA:palette.tga")
magick("checker_tmp.png", "-type", "Grayscale", "-compress", "RLE", "TGA:grey_rle.tga")
magick("grad_tmp.png", "PCX:grad.pcx")
magick("pal_tmp.png", "-type", "Palette", "PCX:palette.pcx")
# ImageMagick stores black as 1 in monochrome PCX; Pillow follows ZSoft (1 = white).
checker.convert("1").save("mono.pcx")
# HDR, farbfeld
magick("wide_tmp.ppm", "-colorspace", "RGB", "HDR:wide.hdr")
magick("wide_tmp.ppm", "FF:wide.ff")
# WBMP, XBM, XPM
magick("checker_tmp.png", "WBMP:checker.wbmp")
magick("checker_tmp.png", "XBM:checker.xbm")
magick("pal_tmp.png", "-type", "Palette", "XPM:palette.xpm")
magick("alpha_tmp.png", "-alpha", "on", "-channel", "A", "-threshold", "50%", "+channel", "XPM:alpha.xpm")
# SGI, Sun
magick("grad_tmp.png", "-compress", "RLE", "SGI:grad_rle.sgi")
magick("alpha_tmp.png", "-compress", "None", "SGI:alpha.sgi")
magick("wide_tmp.ppm", "-depth", "16", "SGI:wide16.sgi")
magick("grad_tmp.png", "SUN:grad.ras")
magick("pal_tmp.png", "-type", "Palette", "SUN:palette.ras")
magick("checker_tmp.png", "-type", "Bilevel", "SUN:mono.ras")

for name in ("grad_tmp.png", "alpha_tmp.png", "pal_tmp.png", "checker_tmp.png", "wide_tmp.ppm"):
    run("rm", name)
