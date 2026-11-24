#!/usr/bin/env python3
"""Texture fixtures, step 1 (scratch venv with etcpak, texture2ddecoder,
srctools, Pillow): compress the synthetic source pictures with etcpak (BCn,
ETC, EAC blocks), decode every block stream with texture2ddecoder (the
independent reference), write Valve VTF files with srctools and the PVR v3
containers (simple header around the etcpak / astcenc blocks).

Usage: python make_textures.py   (in the directory holding src_rgba.png)
Step 2 (KTX-Software, astcenc, Compressonator) is in SOURCES.md.
"""
import struct

import etcpak
import texture2ddecoder as t2d
from PIL import Image
from srctools import vtf

src = Image.open("src_rgba.png").convert("RGBA")
w, h = src.size
rgba = src.tobytes()
bgra = Image.frombytes("RGBA", (w, h), rgba).tobytes("raw", "BGRA")


def save_ref(name, data, ww=w, hh=h):
    Image.frombytes("RGBA", (ww, hh), data, "raw", "BGRA").save(name + "_ref.png")


# etcpak takes RGBA input.
codecs = {
    "bc1": (etcpak.compress_bc1, lambda d: t2d.decode_bc1(d, w, h)),
    "bc3": (etcpak.compress_bc3, lambda d: t2d.decode_bc3(d, w, h)),
    "bc4": (etcpak.compress_bc4, lambda d: t2d.decode_bc4(d, w, h)),
    "bc5": (etcpak.compress_bc5, lambda d: t2d.decode_bc5(d, w, h)),
    "bc7": (etcpak.compress_bc7, lambda d: t2d.decode_bc7(d, w, h)),
    "etc1": (etcpak.compress_etc1_rgb, lambda d: t2d.decode_etc1(d, w, h)),
    "etc2": (etcpak.compress_etc2_rgb, lambda d: t2d.decode_etc2(d, w, h)),
    "etc2a8": (etcpak.compress_etc2_rgba, lambda d: t2d.decode_etc2a8(d, w, h)),
    "eacrg": (etcpak.compress_eac_rg, lambda d: t2d.decode_eacrg(d, w, h)),
}
blocks = {}
for name, (encode, decode) in codecs.items():
    data = encode(rgba, w, h)
    blocks[name] = data
    open(name + ".raw", "wb").write(data)
    save_ref(name, decode(data))


def pvr3(path, pixel_format, data, colour_space=0, channel_type=0, width=w, height=h):
    header = struct.pack("<IIQIIIIIIIII", 0x03525650, 0, pixel_format, colour_space, channel_type, height, width, 1,
                         1, 1, 1, 0)
    open(path, "wb").write(header + data)


# EAC R11: the red halves of the RG11 blocks (etcpak's own R11 output uses
# 16-byte blocks).
rg = blocks["eacrg"]
blocks["eacr"] = b"".join(rg[i:i + 8] for i in range(0, len(rg), 16))
open("eacr.raw", "wb").write(blocks["eacr"])
save_ref("eacr", t2d.decode_eacr(blocks["eacr"], w, h))

pvr3("rgba8888.pvr", (8 << 32 | 8 << 40 | 8 << 48 | 8 << 56) | ord("r") | ord("g") << 8 | ord("b") << 16 | ord("a") << 24,
     rgba)
pvr3("etc1.pvr", 6, blocks["etc1"])
pvr3("dxt1.pvr", 7, blocks["bc1"])
pvr3("bc7.pvr", 15, blocks["bc7"])
pvr3("etc2a8.pvr", 23, blocks["etc2a8"])
# ASTC 6x6 blocks from astcenc (astc6x6.astc, written in step 2 before this runs).
try:
    astc = open("astc6x6.astc", "rb").read()
    pvr3("astc6x6.pvr", 27 + 4, astc[16:])
except FileNotFoundError:
    pass

# Valve VTF with srctools (power-of-two sizes only: the top 64x32 of the
# source): plain and DXT formats, versions 7.2 to 7.5, two frames.
vsrc = src.crop((0, 0, 64, 32))
for fmt, version, frames in [("RGBA8888", (7, 5), 1), ("BGRA8888", (7, 2), 1), ("DXT1", (7, 5), 1),
                             ("DXT5", (7, 2), 2), ("RGB888", (7, 4), 1), ("I8", (7, 5), 1)]:
    tex = vtf.VTF(64, 32, version, frames=frames, fmt=vtf.ImageFormats[fmt])
    for f in range(frames):
        frame = vsrc if f == 0 else vsrc.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        tex.get(frame=f).copy_from(frame.tobytes(), vtf.ImageFormats.RGBA8888)
    tex.compute_mipmaps()
    name = "%s_%d%d" % (fmt.lower(), version[0], version[1])
    with open(name + ".vtf", "wb") as f:
        tex.save(f, version)
    # srctools' own decoding of what it wrote, frame by frame.
    with open(name + ".vtf", "rb") as f:
        back = vtf.VTF.read(f)
        for fr in range(frames):
            img = back.get(frame=fr).to_PIL()
            img.save("%s_ref%d.png" % (name, fr))
print("ok")
