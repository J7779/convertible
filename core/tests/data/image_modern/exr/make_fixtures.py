"""Write the OpenEXR test fixtures with the reference OpenEXR library.

Run from this directory with a Python that has the OpenEXR (3.5.x) and
numpy packages:  python make_fixtures.py
All pictures are synthetic test patterns. The *.ref files hold what the
reference library decodes from the lossy fixtures (little-endian half
floats, channels interleaved, rows top to bottom).
"""
import numpy as np
import OpenEXR

W, H = 37, 23
y, x = np.mgrid[0:H, 0:W].astype(np.float64)


def rgb_pattern(xx, yy):
    return xx / 8.0, yy / 4.0, 0.25 + (xx + yy) / 64.0


def alpha_pattern(xx, yy):
    return ((xx + yy) % 4 + 1) / 4.0


def write(path, channels, **header):
    header.setdefault("compression", OpenEXR.ZIP_COMPRESSION)
    header.setdefault("type", OpenEXR.scanlineimage)
    OpenEXR.File(header, channels).write(path)


def half(a):
    return np.ascontiguousarray(a.astype(np.float16))


def ref(path, names):
    f = OpenEXR.File(path, separate_channels=True)
    ch = f.channels()
    planes = [ch[n].pixels.astype(np.float16) for n in names]
    np.stack(planes, axis=-1).astype("<f2").tofile(path.replace(".exr", ".ref"))


R, G, B = rgb_pattern(x, y)
A = alpha_pattern(x, y)
rgb = {"R": half(R), "G": half(G), "B": half(B)}

# One file per compression method, RGB half.
methods = {
    "none": OpenEXR.NO_COMPRESSION, "rle": OpenEXR.RLE_COMPRESSION, "zips": OpenEXR.ZIPS_COMPRESSION,
    "zip": OpenEXR.ZIP_COMPRESSION, "piz": OpenEXR.PIZ_COMPRESSION, "pxr24": OpenEXR.PXR24_COMPRESSION,
    "b44": OpenEXR.B44_COMPRESSION, "b44a": OpenEXR.B44A_COMPRESSION, "dwaa": OpenEXR.DWAA_COMPRESSION,
    "dwab": OpenEXR.DWAB_COMPRESSION,
}
for name, method in methods.items():
    path = f"rgb_half_{name}.exr"
    write(path, rgb, compression=method)
    if name in ("b44", "b44a", "dwaa", "dwab"):
        ref(path, ["R", "G", "B"])

# Busy content for the lossy codecs: B44 (half) and DWAB (float channels).
noise = ((x * 7919 + y * 104729) % 1000) / 250.0
busy = {"R": noise, "G": (noise + x / 16.0) % 3.0, "B": 0.5 + np.sin(x / 3.0) * np.cos(y / 2.0)}
write("noise_b44.exr", {k: half(v) for k, v in busy.items()}, compression=OpenEXR.B44_COMPRESSION)
ref("noise_b44.exr", ["R", "G", "B"])
write("noise_float_dwab.exr", {k: np.ascontiguousarray(v.astype(np.float32)) for k, v in busy.items()},
      compression=OpenEXR.DWAB_COMPRESSION)
ref("noise_float_dwab.exr", ["R", "G", "B"])

# RGBA (premultiplied, exact in half) with PIZ and B44A; RGBA + Z with DWAA
# (lossy colour, RLE alpha, zlib Z).
rgba = dict(rgb, R=half(R * A), G=half(G * A), B=half(B * A), A=half(A))
write("rgba_half_piz.exr", rgba, compression=OpenEXR.PIZ_COMPRESSION)
rgbaz = dict(rgba, Z=np.ascontiguousarray((x * 0.5 + y).astype(np.float32)))
write("rgbaz_dwaa.exr", rgbaz, compression=OpenEXR.DWAA_COMPRESSION)
ref("rgbaz_dwaa.exr", ["R", "G", "B", "A"])

# 32-bit float and unsigned int channels.
flt = {"R": np.ascontiguousarray((x / 3.0).astype(np.float32)),
       "G": np.ascontiguousarray((y * 0.1).astype(np.float32)),
       "B": np.ascontiguousarray((1000.5 + x).astype(np.float32))}
write("rgb_float_zip.exr", flt, compression=OpenEXR.ZIP_COMPRESSION)
write("rgb_float_piz.exr", flt, compression=OpenEXR.PIZ_COMPRESSION)
write("rgb_float_pxr24.exr", flt, compression=OpenEXR.PXR24_COMPRESSION)
uint = {"R": np.ascontiguousarray((x * 1000).astype(np.uint32)),
        "G": np.ascontiguousarray(y.astype(np.uint32)),
        "B": np.ascontiguousarray((70000 + x * y).astype(np.uint32))}
write("rgb_uint_rle.exr", uint, compression=OpenEXR.RLE_COMPRESSION)

# Tiled, one level.
tiles = OpenEXR.TileDescription()
tiles.xSize, tiles.ySize = 16, 8
tiles.mode, tiles.roundingMode = OpenEXR.ONE_LEVEL, OpenEXR.ROUND_DOWN
write("tiled_zip.exr", rgb, type=OpenEXR.tiledimage, tiles=tiles)

# Lines stored bottom to top.
write("decreasing_zip.exr", rgb, lineOrder=OpenEXR.DECREASING_Y)

# Data window (5,3)-(24,20) inside display window (0,0)-(39,29); the
# pattern uses absolute pixel coordinates.
dy, dx = np.mgrid[3:21, 5:25].astype(np.float64)
dR, dG, dB = rgb_pattern(dx, dy)
write("datawindow_zip.exr", {"R": half(dR), "G": half(dG), "B": half(dB)},
      dataWindow=(np.array([5, 3], dtype=np.int32), np.array([24, 20], dtype=np.int32)),
      displayWindow=(np.array([0, 0], dtype=np.int32), np.array([39, 29], dtype=np.int32)))

# Luminance with alpha (luminance + subsampled chroma: see make_yca.cc).
Y = x / 8.0 + y / 16.0
write("ya_zip.exr", {"Y": half(Y * A), "A": half(A)})
# A layered file: the "diffuse" layer is the picture, Z is extra.
write("layers_zip.exr", {"diffuse.R": half(R), "diffuse.G": half(G), "diffuse.B": half(B),
                         "Z": np.ascontiguousarray(y.astype(np.float32))})

# Multi-part (parts share the display window): two full parts and a third
# with a 4x4 data window at (10,10) holding a grey value.
p0 = OpenEXR.Part({"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage}, rgb, name="left")
p1 = OpenEXR.Part({"compression": OpenEXR.PIZ_COMPRESSION, "type": OpenEXR.scanlineimage},
                  {"R": half(4.5 - R), "G": half(G), "B": half(B)}, name="right")
small = np.full((4, 4), 0.75, dtype=np.float16)
p2 = OpenEXR.Part({"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage,
                   "dataWindow": (np.array([10, 10], dtype=np.int32), np.array([13, 13], dtype=np.int32)),
                   "displayWindow": (np.array([0, 0], dtype=np.int32), np.array([W - 1, H - 1], dtype=np.int32))},
                  {"Y": small}, name="patch")
OpenEXR.File([p0, p1, p2]).write("multipart.exr")

# ACES AP0 primaries (D60 white): pure AP0 red/green/blue/grey swatches.
ap0 = (0.7347, 0.2653, 0.0, 1.0, 0.0001, -0.077, 0.32168, 0.33767)
sw = np.zeros((2, 4, 3))
sw[:, 0] = (1, 0, 0)
sw[:, 1] = (0, 1, 0)
sw[:, 2] = (0, 0, 1)
sw[:, 3] = (0.5, 0.5, 0.5)
write("aces_zip.exr", {"R": half(sw[..., 0]), "G": half(sw[..., 1]), "B": half(sw[..., 2])},
      chromaticities=ap0)
