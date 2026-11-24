# Regenerates the PSD/PSB and DDS fixtures in this directory (see SOURCES.md).
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

# Photoshop documents (ImageMagick writes a layer plus the merged composite).
magick("grad_tmp.png", "PSD:rgb8.psd")
magick("grad_tmp.png", "-colorspace", "Gray", "PSD:grey8.psd")
magick("grad_tmp.png", "-depth", "16", "PSD:rgb16.psd")
magick("grad_tmp.png", "-colorspace", "CMYK", "PSD:cmyk8.psd")
magick("grad_tmp.png", "-compress", "None", "PSB:rgb8_raw.psb")

# RGBA composite written by hand from the specification: Photoshop stores the
# merged image blended onto white (ImageMagick 7 writes it unblended but reads
# it as blended, so its output cannot serve as a reference here).
planes = [bytearray(), bytearray(), bytearray(), bytearray()]
for y in range(H):
    for x in range(W):
        r, g, b, a = alpha.getpixel((x, y))
        for c, v in enumerate((r, g, b)):
            planes[c].append((v * a + 255 * (255 - a) + 127) // 255)
        planes[3].append(a)
psd = b"8BPS" + struct.pack(">H", 1) + b"\0" * 6 + struct.pack(">HIIHH", 4, H, W, 8, 3)
psd += struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">H", 0)
psd += b"".join(bytes(plane) for plane in planes)
open("rgba8.psd", "wb").write(psd)

# DirectDraw surfaces.
magick("alpha_tmp.png", "-define", "dds:compression=none", "-define", "dds:mipmaps=0", "DDS:rgba.dds")
magick("grad_tmp.png", "-define", "dds:compression=dxt1", "-define", "dds:mipmaps=0", "DDS:dxt1.dds")
magick("alpha_tmp.png", "-define", "dds:compression=dxt5", "-define", "dds:mipmaps=0", "DDS:dxt5.dds")
magick("alpha_tmp.png", "-define", "dds:compression=dxt3", "-define", "dds:mipmaps=0", "DDS:dxt3.dds")

# DX10 header with DXGI_FORMAT_R8G8B8A8_UNORM (28), written by hand from the specification.
pixels = b"".join(struct.pack("BBBB", *alpha.getpixel((x, y))) for y in range(H) for x in range(W))
header = struct.pack("<4sIIIIIII", b"DDS ", 124, 0x1007 | 0x8, H, W, W * 4, 0, 1) + b"\0" * 44
header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
header += struct.pack("<IIIII", 28, 3, 0, 1, 0)
open("dx10_rgba.dds", "wb").write(header + pixels)

for name in ("grad_tmp.png", "alpha_tmp.png"):
    run("rm", name)
