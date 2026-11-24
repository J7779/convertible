#!/usr/bin/env python3
"""Synthetic camera RAW fixtures for the image_modern raw tests.

Run from this directory with a Python that has numpy, Pillow, imagecodecs and
rawpy installed (a scratch virtual environment):

    python make_fixtures.py

Every file is a synthetic test pattern (four flat colour patches) written
with this script's own TIFF / DNG writer. Lossless JPEG streams are checked
with imagecodecs (liblj92) where it supports them; each RAW file is then
decoded with LibRaw (through rawpy) as an independent reference: its raw
sample values must equal the values written here, and its developed linear
sRGB output is saved as a 16-bit PPM reference (`*.libraw.ppm`).
"""

import heapq
import io
import struct
import sys

import numpy as np
from PIL import Image

try:
    import imagecodecs
except ImportError:  # optional cross-check
    imagecodecs = None
try:
    import rawpy
except ImportError:
    rawpy = None

# --------------------------------------------------------------------------- scene

# Linear sRGB colours of the four patches (top-left, top-right, bottom-left,
# bottom-right). Kept below clipping after white balance.
PATCHES = [(0.60, 0.25, 0.10), (0.10, 0.45, 0.20), (0.12, 0.18, 0.62), (0.40, 0.40, 0.40)]

XYZ_FROM_SRGB = np.array([[0.4124564, 0.3575761, 0.1804375],
                          [0.2126729, 0.7151522, 0.0721750],
                          [0.0193339, 0.1191920, 0.9503041]])
# A made-up but plausible camera: XYZ -> camera RGB.
COLOR_MATRIX = np.array([[0.80, -0.20, -0.05], [-0.40, 1.20, 0.20], [-0.05, 0.15, 0.60]])
NEUTRAL = np.array([0.50, 1.00, 0.70])


def scene(width, height):
    """Linear sRGB picture of the four patches, shape (h, w, 3)."""
    img = np.zeros((height, width, 3))
    for y in range(height):
        for x in range(width):
            img[y, x] = PATCHES[(1 if x >= width // 2 else 0) + (2 if y >= height // 2 else 0)]
    return img


def camera_from_srgb():
    m = COLOR_MATRIX @ XYZ_FROM_SRGB
    return m / m.sum(axis=1, keepdims=True)


def to_camera(img, neutral=NEUTRAL):
    """Linear sRGB -> normalised camera values (before white balance)."""
    bal = img @ camera_from_srgb().T
    gains = (1.0 / neutral) / (1.0 / neutral).min()
    return bal / gains


def mosaic(cam, pattern):
    """CFA plane from a camera image; `pattern` is a 2-D array of 0/1/2."""
    h, w, _ = cam.shape
    ph, pw = pattern.shape
    out = np.zeros((h, w))
    for y in range(h):
        for x in range(w):
            out[y, x] = cam[y, x, pattern[y % ph, x % pw]]
    return out


def quantise(plane, black, white):
    return np.clip(np.round(black + plane * (white - black)), 0, 65535).astype(np.uint16)


# --------------------------------------------------------------------------- TIFF writer

BYTE, ASCII, SHORT, LONG, RATIONAL, UNDEFINED, SRATIONAL, FLOAT, DOUBLE = 1, 2, 3, 4, 5, 7, 10, 11, 12
SIZES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8, 11: 4, 12: 8}


class Tiff:
    """Sequential TIFF builder: blobs first, then directories (children first)."""

    def __init__(self, order="<", header=None):
        self.o = order
        self.buf = bytearray(header if header is not None else
                             (b"II*\x00" if order == "<" else b"MM\x00*") + b"\x00\x00\x00\x00")

    def align(self):
        if len(self.buf) % 2:
            self.buf += b"\x00"

    def blob(self, data):
        self.align()
        at = len(self.buf)
        self.buf += data
        return at

    def encode(self, typ, values):
        o = self.o
        if typ == ASCII:
            data = values.encode("ascii") + b"\x00"
            return data, len(data)
        if typ in (UNDEFINED, BYTE) and isinstance(values, (bytes, bytearray)):
            return bytes(values), len(values)
        if not isinstance(values, (list, tuple)):
            values = [values]
        if typ in (RATIONAL, SRATIONAL):
            data = b""
            for v in values:
                if isinstance(v, tuple):
                    num, den = v
                else:
                    den = 1000000
                    num = int(round(v * den))
                data += struct.pack(o + ("II" if typ == RATIONAL else "ii"), num, den)
            return data, len(values)
        fmt = {BYTE: "B", SHORT: "H", LONG: "I", FLOAT: "f", DOUBLE: "d", UNDEFINED: "B"}[typ]
        return struct.pack(o + fmt * len(values), *values), len(values)

    def ifd(self, entries, next_ifd=0):
        """Write a directory; returns (offset, end)."""
        self.align()
        entries = sorted(entries, key=lambda e: e[0])
        start = len(self.buf)
        table = 2 + 12 * len(entries) + 4
        ext = bytearray()
        rows = []
        for entry in entries:
            tag, typ, values = entry[0], entry[1], entry[2]
            if typ == "at":  # value stored elsewhere: (tag, "at", (type, count, offset))
                t2, count, offset = values
                rows.append(struct.pack(self.o + "HHII", tag, t2, count, offset))
                continue
            data, count = self.encode(typ, values)
            if len(data) <= 4:
                rows.append(struct.pack(self.o + "HHI", tag, typ, count) + data.ljust(4, b"\x00"))
            else:
                if len(ext) % 2:
                    ext += b"\x00"
                rows.append(struct.pack(self.o + "HHII", tag, typ, count, start + table + len(ext)))
                ext += data
        self.buf += struct.pack(self.o + "H", len(entries)) + b"".join(rows) + struct.pack(self.o + "I", next_ifd) + ext
        return start, len(self.buf)

    def set_first(self, offset, at=4):
        struct.pack_into(self.o + "I", self.buf, at, offset)

    def bytes(self):
        return bytes(self.buf)


# --------------------------------------------------------------------------- lossless JPEG encoder

def huffman_lengths(freq):
    """JPEG Annex K.2: code lengths (max 16) for symbols 0..16."""
    freq = list(freq) + [1]  # reserved symbol so that no code is all ones
    n = len(freq)
    codesize = [0] * n
    others = [-1] * n
    f = freq[:]
    while True:
        live = sorted([i for i in range(n) if f[i] > 0], key=lambda i: (f[i], -i))
        if len(live) < 2:
            break
        v1, v2 = live[0], live[1]
        f[v1] += f[v2]
        f[v2] = 0
        codesize[v1] += 1
        while others[v1] != -1:
            v1 = others[v1]
            codesize[v1] += 1
        others[v1] = v2
        codesize[v2] += 1
        while others[v2] != -1:
            v2 = others[v2]
            codesize[v2] += 1
    bits = [0] * 33
    for i in range(n):
        if codesize[i]:
            bits[codesize[i]] += 1
    for i in range(32, 16, -1):
        while bits[i] > 0:
            j = i - 2
            while bits[j] == 0:
                j -= 1
            bits[i] -= 2
            bits[i - 1] += 1
            bits[j + 1] += 2
            bits[j] -= 1
    i = 16
    while bits[i] == 0:
        i -= 1
    bits[i] -= 1  # remove the reserved symbol
    order = sorted([s for s in range(n - 1) if freq[s] > 0], key=lambda s: (codesize[s], s))
    return bits[1:17], order


def lj92_encode(rows, components, precision, predictor, restart=0):
    """rows: (H, W*components) uint16. Returns a SOI..EOI stream."""
    rows = rows.astype(np.int64)
    h, total = rows.shape
    w = total // components
    initial = 1 << (precision - 1)
    diffs = np.zeros_like(rows)
    mcu = 0
    first_line = True
    reset = True
    for y in range(h):
        for x in range(w):
            if restart and mcu and mcu % restart == 0:
                reset = True
                first_line = True
            for c in range(components):
                i = x * components + c
                if reset:
                    pred = initial
                elif first_line or y == 0:
                    pred = rows[y, i - components] if x > 0 else initial
                elif x == 0:
                    pred = rows[y - 1, i]
                else:
                    ra, rb, rc = rows[y, i - components], rows[y - 1, i], rows[y - 1, i - components]
                    pred = {1: ra, 2: rb, 3: rc, 4: ra + rb - rc, 5: ra + ((rb - rc) >> 1),
                            6: rb + ((ra - rc) >> 1), 7: (ra + rb) >> 1}[predictor]
                d = (int(rows[y, i]) - int(pred)) & 0xFFFF
                if d >= 32768:
                    d -= 65536
                diffs[y, i] = d
            reset = False
            mcu += 1
        first_line = False

    def category(d):
        return 16 if d == -32768 else int(abs(int(d))).bit_length()

    freq = [0] * 17
    for d in diffs.flat:
        freq[category(d)] += 1
    bits, order = huffman_lengths(freq)
    codes = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[order[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    out = bytearray()
    acc = 0
    nbits = 0

    def put(value, length):
        nonlocal acc, nbits
        acc = (acc << length) | (value & ((1 << length) - 1))
        nbits += length
        while nbits >= 8:
            byte = (acc >> (nbits - 8)) & 0xFF
            out.append(byte)
            if byte == 0xFF:
                out.append(0)
            nbits -= 8

    def flush():
        nonlocal acc, nbits
        if nbits:
            put(0x7F, 8 - nbits)
        acc = 0

    mcu = 0
    rst = 0
    for y in range(h):
        for x in range(w):
            if restart and mcu and mcu % restart == 0:
                flush()
                out += bytes([0xFF, 0xD0 + (rst & 7)])
                rst += 1
            for c in range(components):
                d = int(diffs[y, x * components + c])
                s = category(d)
                cv, cl = codes[s]
                put(cv, cl)
                if 0 < s < 16:
                    put(d if d > 0 else d + (1 << s) - 1, s)
            mcu += 1
    flush()
    seg = bytearray(b"\xff\xd8")
    sof = struct.pack(">BHHB", precision, h, w, components)
    for c in range(components):
        sof += bytes([c + 1, 0x11, 0])
    seg += b"\xff\xc3" + struct.pack(">H", len(sof) + 2) + sof
    dht = bytes([0x00]) + bytes(bits) + bytes(order)
    seg += b"\xff\xc4" + struct.pack(">H", len(dht) + 2) + dht
    if restart:
        seg += b"\xff\xdd" + struct.pack(">HH", 4, restart)
    sos = bytes([components])
    for c in range(components):
        sos += bytes([c + 1, 0x00])
    sos += bytes([predictor, 0, 0])
    seg += b"\xff\xda" + struct.pack(">H", len(sos) + 2) + sos
    seg += out + b"\xff\xd9"
    return bytes(seg)


def check_lj92(stream, rows):
    if imagecodecs is None:
        return
    try:
        decoded = imagecodecs.ljpeg_decode(stream)
    except Exception as exc:  # liblj92 does not handle every layout
        print("  (imagecodecs could not cross-check:", exc, ")")
        return
    assert np.array_equal(np.asarray(decoded).reshape(rows.shape), rows), "liblj92 disagrees"


# --------------------------------------------------------------------------- helpers

def jpeg(width, height, quality=85, color=(200, 120, 60)):
    img = Image.new("RGB", (width, height), color)
    for x in range(width // 2):
        for y in range(height):
            img.putpixel((x, y), (40, 90, 200))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=quality)
    return out.getvalue()


def pack12_msb(values):
    v = values.astype(np.uint32).reshape(-1)
    out = bytearray()
    for i in range(0, len(v), 2):
        a, b = int(v[i]), int(v[i + 1])
        out += bytes([a >> 4, ((a & 15) << 4) | (b >> 8), b & 0xFF])
    return bytes(out)


def u16(values, order="<"):
    return values.astype(order + "u2").tobytes()


def write_ppm16(path, rgb16):
    h, w, _ = rgb16.shape
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n65535\n" % (w, h))
        f.write(rgb16.astype(">u2").tobytes())


def libraw_check(path, expected_raw=None, ref_name=None):
    if rawpy is None:
        print("  rawpy missing: no reference check for", path)
        return
    with rawpy.imread(path) as raw:
        if expected_raw is not None:
            got = raw.raw_image.copy()
            assert got.shape == expected_raw.shape, (got.shape, expected_raw.shape)
            assert np.array_equal(got, expected_raw), "LibRaw raw values differ for " + path
        if ref_name:
            rgb = raw.postprocess(use_camera_wb=True, output_color=rawpy.ColorSpace.sRGB, gamma=(1, 1),
                                  no_auto_bright=True, output_bps=16, highlight_mode=rawpy.HighlightMode.Clip,
                                  user_flip=0)
            write_ppm16(ref_name, rgb)
    print("  LibRaw ok:", path)


DNG_COMMON = lambda: [
    (50706, BYTE, [1, 4, 0, 0]),           # DNGVersion
    (50707, BYTE, [1, 1, 0, 0]),           # DNGBackwardVersion
    (271, ASCII, "Synthetic"),             # Make
    (272, ASCII, "Test Pattern"),          # Model
    (50708, ASCII, "Synthetic Test Pattern"),
    (50721, SRATIONAL, list(COLOR_MATRIX.flatten())),
    (50778, SHORT, [21]),                  # CalibrationIlluminant1 = D65
    (50728, RATIONAL, list(NEUTRAL)),      # AsShotNeutral
]

RGGB = np.array([[0, 1], [1, 2]])


def cfa_tags(pattern):
    return [(33421, SHORT, [pattern.shape[0], pattern.shape[1]]),
            (33422, BYTE, bytes(int(v) for v in pattern.flatten())),
            (50710, BYTE, bytes([0, 1, 2])), (50711, SHORT, [1])]


# --------------------------------------------------------------------------- fixtures

def dng_uncompressed16():
    """64x40 sensor, active area (2, 4, 40, 64), RGGB, 16-bit strips."""
    W, H, top, left = 64, 40, 2, 4
    black, white = 256, 60000
    aw, ah = W - left, H - top
    plane = mosaic(to_camera(scene(aw, ah)), RGGB)  # pattern anchored at the active area
    raw = np.full((H, W), black, np.uint16)
    raw[top:, left:] = quantise(plane, black, white)
    # The DNG CFA pattern is anchored at the sensor origin: shift (left, top) is even, so RGGB.
    t = Tiff()
    strips = [t.blob(u16(raw[y:y + 16])) for y in range(0, H, 16)]
    counts = [len(u16(raw[y:y + 16])) for y in range(0, H, 16)]
    entries = DNG_COMMON() + cfa_tags(RGGB) + [
        (254, LONG, [0]), (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [16]), (259, SHORT, [1]),
        (262, SHORT, [32803]), (273, LONG, strips), (277, SHORT, [1]), (278, LONG, [16]), (279, LONG, counts),
        (284, SHORT, [1]), (274, SHORT, [1]),
        (50714, SHORT, [black]), (50717, SHORT, [white]),
        (50829, LONG, [top, left, H, W]),
        (50719, LONG, [2, 2]), (50720, LONG, [aw - 4, ah - 4]),
    ]
    prev = jpeg(30, 18)
    poff = t.blob(prev)
    sub, _ = t.ifd(entries)
    # IFD0: a JPEG preview (NewSubFileType 1) with the raw as SubIFD.
    ifd0 = DNG_COMMON() + [
        (254, LONG, [1]), (256, LONG, [30]), (257, LONG, [18]), (258, SHORT, [8, 8, 8]), (259, SHORT, [7]),
        (262, SHORT, [6]), (273, LONG, [poff]), (277, SHORT, [3]), (278, LONG, [18]), (279, LONG, [len(prev)]),
        (274, SHORT, [1]), (330, LONG, [sub]),
    ]
    off0, _ = t.ifd(ifd0)
    t.set_first(off0)
    open("dng_cfa_u16_strips.dng", "wb").write(t.bytes())
    libraw_check("dng_cfa_u16_strips.dng", raw, "dng_cfa_u16_strips.libraw.ppm")


def dng_lj92_tiles():
    """48x32, GRBG, 14-bit lossless JPEG tiles (16x16, two components per
    frame, predictor 1..7 cycling over the tiles, one tile with restart markers)."""
    W, H = 48, 32
    GRBG = np.array([[1, 0], [2, 1]])
    black, white = 512, 16383
    plane = mosaic(to_camera(scene(W, H)), GRBG)
    raw = quantise(plane, black, white)
    t = Tiff()
    offsets, counts = [], []
    k = 0
    for ty in range(0, H, 16):
        for tx in range(0, W, 16):
            tile = raw[ty:ty + 16, tx:tx + 16]
            predictor = 1 if k == 4 else k % 6 + 2 if k > 4 else k % 7 + 1
            restart = 8 if k == 4 else 0
            stream = lj92_encode(tile, 2, 14, predictor, restart)
            if k == 0:
                check_lj92(stream, tile)
            offsets.append(t.blob(stream))
            counts.append(len(stream))
            k += 1
    entries = DNG_COMMON() + cfa_tags(GRBG) + [
        (254, LONG, [0]), (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [14]), (259, SHORT, [7]),
        (262, SHORT, [32803]), (277, SHORT, [1]), (284, SHORT, [1]), (274, SHORT, [1]),
        (322, SHORT, [16]), (323, SHORT, [16]), (324, LONG, offsets), (325, LONG, counts),
        (50714, SHORT, [black]), (50717, SHORT, [white]),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_cfa_lj92_tiles.dng", "wb").write(t.bytes())
    libraw_check("dng_cfa_lj92_tiles.dng", raw, "dng_cfa_lj92_tiles.libraw.ppm")


def dng_packed12_orientation():
    """40x28 BGGR 12-bit packed, 2x2 black pattern, linearisation table,
    orientation 6 (rotate 90 degrees clockwise)."""
    W, H = 40, 28
    BGGR = np.array([[2, 1], [1, 0]])
    blacks = np.array([[60, 64], [66, 70]])
    white = 4000
    # Linearisation: stored code v -> 2*v (so stored values are half the linear ones).
    table = [min(2 * v, 65535) for v in range(4096)]
    plane = mosaic(to_camera(scene(W, H)), BGGR)
    lin = np.zeros((H, W))
    for y in range(H):
        for x in range(W):
            b = blacks[y % 2, x % 2]
            lin[y, x] = b + plane[y, x] * (white - b)
    stored = np.clip(np.round(lin / 2), 0, 4095).astype(np.uint16)
    t = Tiff()
    data = pack12_msb(stored)
    off = t.blob(data)
    entries = DNG_COMMON() + cfa_tags(BGGR) + [
        (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [12]), (259, SHORT, [1]),
        (262, SHORT, [32803]), (273, LONG, [off]), (277, SHORT, [1]), (278, LONG, [H]), (279, LONG, [len(data)]),
        (274, SHORT, [6]),
        (50712, SHORT, table), (50713, SHORT, [2, 2]), (50714, SHORT, [int(v) for v in blacks.flatten()]),
        (50717, SHORT, [white]),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_cfa_packed12_rot90.dng", "wb").write(t.bytes())
    try:
        libraw_check("dng_cfa_packed12_rot90.dng", stored, None)
    except AssertionError:
        libraw_check("dng_cfa_packed12_rot90.dng", (stored.astype(np.uint32) * 2).astype(np.uint16), None)


def dng_linear_float_deflate():
    """32x24 LinearRaw RGB, 32-bit float, Deflate with the floating-point
    predictor, 16x16 tiles; forward matrix, baseline exposure +1."""
    import zlib
    W, H = 32, 24
    cam = to_camera(scene(W, H)) / 2.0  # baseline exposure +1 brings it back
    t = Tiff()
    offsets, counts = [], []
    for ty in range(0, H, 16):
        for tx in range(0, W, 16):
            tile = np.zeros((16, 16, 3), np.float32)
            part = cam[ty:ty + 16, tx:tx + 16].astype(np.float32)
            tile[:part.shape[0], :part.shape[1]] = part
            rows = []
            for r in range(16):
                be = tile[r].reshape(-1).astype(">f4").tobytes()
                n = 16 * 3
                planes = bytearray(4 * n)
                for i in range(n):
                    for b in range(4):
                        planes[b * n + i] = be[i * 4 + b]
                for i in range(len(planes) - 1, 2, -1):  # difference with distance spp = 3
                    planes[i] = (planes[i] - planes[i - 3]) & 0xFF
                rows.append(bytes(planes))
            comp = zlib.compress(b"".join(rows), 9)
            offsets.append(t.blob(comp))
            counts.append(len(comp))
    # Forward matrix for white-balanced camera RGB -> XYZ (D50): built from the
    # camera model above so that the expected sRGB is the scene.
    bradford_d65_to_d50 = np.array([[1.0478112, 0.0228866, -0.0501270],
                                    [0.0295424, 0.9904844, -0.0170491],
                                    [-0.0092345, 0.0150436, 0.7521316]])
    fm = bradford_d65_to_d50 @ XYZ_FROM_SRGB @ np.linalg.inv(camera_from_srgb())
    entries = DNG_COMMON() + [
        (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [32, 32, 32]), (259, SHORT, [8]),
        (262, SHORT, [34892]), (277, SHORT, [3]), (284, SHORT, [1]), (317, SHORT, [3]), (339, SHORT, [3, 3, 3]),
        (322, SHORT, [16]), (323, SHORT, [16]), (324, LONG, offsets), (325, LONG, counts),
        (50717, LONG, [1]), (50730, SRATIONAL, [1.0]), (50964, SRATIONAL, list(fm.flatten())),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_linear_float_deflate.dng", "wb").write(t.bytes())
    print("  float DNG written (LibRaw reference not used for float data)")


def dng_linear_lossy():
    """32x24 LinearRaw RGB 8-bit, lossy JPEG tiles (Pillow), white level 255."""
    W, H = 32, 24
    cam = to_camera(scene(W, H))
    v = np.clip(np.round(cam * 255), 0, 255).astype(np.uint8)
    t = Tiff()
    offsets, counts = [], []
    for ty in range(0, H, 16):
        for tx in range(0, W, 16):
            tile = np.zeros((16, 16, 3), np.uint8)
            part = v[ty:ty + 16, tx:tx + 16]
            tile[:part.shape[0], :part.shape[1]] = part
            out = io.BytesIO()
            Image.fromarray(tile, "RGB").save(out, "JPEG", quality=95, subsampling=0)
            offsets.append(t.blob(out.getvalue()))
            counts.append(len(out.getvalue()))
    entries = DNG_COMMON() + [
        (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [8, 8, 8]), (259, SHORT, [34892]),
        (262, SHORT, [34892]), (277, SHORT, [3]), (284, SHORT, [1]),
        (322, SHORT, [16]), (323, SHORT, [16]), (324, LONG, offsets), (325, LONG, counts),
        (50717, SHORT, [255, 255, 255]),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_linear_lossy_jpeg.dng", "wb").write(t.bytes())


def dng_xtrans():
    """36x24 X-Trans 6x6 pattern, 16-bit; two (identical) colour matrices for
    illuminants A and D65, which exercises the illuminant interpolation."""
    W, H = 36, 24
    XT = np.array([[1, 1, 0, 1, 1, 2], [1, 1, 2, 1, 1, 0], [2, 0, 1, 0, 2, 1],
                   [1, 1, 2, 1, 1, 0], [1, 1, 0, 1, 1, 2], [0, 2, 1, 2, 0, 1]])
    black, white = 1024, 16383
    raw = quantise(mosaic(to_camera(scene(W, H)), XT), black, white)
    t = Tiff()
    off = t.blob(u16(raw))
    entries = DNG_COMMON() + cfa_tags(XT) + [
        (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [16]), (259, SHORT, [1]), (262, SHORT, [32803]),
        (273, LONG, [off]), (277, SHORT, [1]), (278, LONG, [H]), (279, LONG, [W * H * 2]),
        (50714, SHORT, [black]), (50717, SHORT, [white]),
        (50722, SRATIONAL, list(COLOR_MATRIX.flatten())), (50779, SHORT, [17]),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_cfa_xtrans.dng", "wb").write(t.bytes())
    libraw_check("dng_cfa_xtrans.dng", raw, None)


def gain_map(top, left, bottom, right, row_pitch, col_pitch, gain):
    """A DNG GainMap opcode (id 9) with a constant 2x2 map."""
    params = struct.pack(">10I", top, left, bottom, right, 0, 1, row_pitch, col_pitch, 2, 2)
    params += struct.pack(">4d", 1.0, 1.0, 0.0, 0.0) + struct.pack(">I", 1) + struct.pack(">4f", *([gain] * 4))
    return struct.pack(">IIII", 9, 0x01030000, 0, len(params)) + params


def dng_gainmap():
    """32x24 RGGB 16-bit with an OpcodeList2 of two gain maps: x2 on the red
    photosites (row/column pitch 2) and x1.25 everywhere; the stored data is
    darkened accordingly."""
    W, H = 32, 24
    black, white = 0, 65535
    plane = mosaic(to_camera(scene(W, H)), RGGB) / 1.25
    plane[0::2, 0::2] /= 2.0
    raw = quantise(plane, black, white)
    opcodes = struct.pack(">I", 2) + gain_map(0, 0, H, W, 2, 2, 2.0) + gain_map(0, 0, H, W, 1, 1, 1.25)
    t = Tiff()
    off = t.blob(u16(raw))
    entries = DNG_COMMON() + cfa_tags(RGGB) + [
        (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [16]), (259, SHORT, [1]), (262, SHORT, [32803]),
        (273, LONG, [off]), (277, SHORT, [1]), (278, LONG, [H]), (279, LONG, [W * H * 2]),
        (50714, SHORT, [black]), (50717, SHORT, [white]), (51009, UNDEFINED, opcodes),
    ]
    off0, _ = t.ifd(entries)
    t.set_first(off0)
    open("dng_cfa_gainmap.dng", "wb").write(t.bytes())
    libraw_check("dng_cfa_gainmap.dng", raw, None)


def cr2():
    """Canon CR2 structure: IFD0 JPEG preview, IFD1 thumbnail, IFD2 RGB,
    IFD3 lossless JPEG in three vertical slices (16, 16, 24 wide), maker
    note with SensorInfo (borders) and ColorData (as-shot RGGB levels)."""
    W, H = 56, 36
    left, top = 8, 4  # sensor borders: active area x 8..55, y 4..35
    black, white = 1024, 16383  # CR2 records no white level: full 14-bit range
    gains = (1.0 / NEUTRAL) / (1.0 / NEUTRAL).min()  # R, G, B multipliers
    cam = to_camera(scene(W - left, H - top))
    raw = np.full((H, W), black, np.uint16)
    raw[top:, left:] = quantise(mosaic(cam, RGGB), black, white)
    # Slices: data is stored slice by slice, each slice row-major.
    slices = [(0, 16), (16, 32), (32, 56)]
    stream_vals = np.concatenate([raw[:, a:b].reshape(-1) for a, b in slices])
    rows = stream_vals.reshape(H, W)  # frame: W/2 wide, 2 components, H high
    stream = lj92_encode(rows, 2, 14, 1)
    t = Tiff(header=b"II*\x00\x10\x00\x00\x00CR\x02\x00\x00\x00\x00\x00")
    prev = jpeg(48, 32)
    thumb = jpeg(16, 10)
    p_off = t.blob(prev)
    t_off = t.blob(thumb)
    rgb = np.full((6, 8, 3), 128, np.uint8)
    r_off = t.blob(rgb.tobytes())
    s_off = t.blob(stream)
    # Maker note (plain IFD, file-relative offsets).
    color = [0] * 582
    color[25:29] = [int(round(1024 * gains[0])), 1024, 1024, int(round(1024 * gains[2]))]
    sensor_info = [34, W, H, 0, 0, left, top, W - 1, H - 1, 1, top, left - 2, H - 1, 0, 0, 0, 0]
    mn_start, mn_end = t.ifd([(0x00E0, SHORT, sensor_info), (0x4001, SHORT, color)])
    exif, _ = t.ifd([(0x927C, "at", (UNDEFINED, mn_end - mn_start, mn_start)), (0x9000, UNDEFINED, b"0230")])
    ifd3, _ = t.ifd([(259, SHORT, [6]), (273, LONG, [s_off]), (279, LONG, [len(stream)]), (0xC5D8, LONG, [1]),
                     (0xC640, SHORT, [2, 16, 24])])
    ifd2, _ = t.ifd([(256, LONG, [8]), (257, LONG, [6]), (258, SHORT, [8, 8, 8]), (259, SHORT, [1]),
                     (262, SHORT, [2]), (273, LONG, [r_off]), (277, SHORT, [3]), (278, LONG, [6]),
                     (279, LONG, [rgb.size])], ifd3)
    ifd1, _ = t.ifd([(259, SHORT, [6]), (513, LONG, [t_off]), (514, LONG, [len(thumb)])], ifd2)
    ifd0, _ = t.ifd([(256, LONG, [48]), (257, LONG, [32]), (258, SHORT, [8, 8, 8]), (259, SHORT, [6]),
                     (271, ASCII, "Canon"), (272, ASCII, "Canon EOS Synthetic"), (273, LONG, [p_off]),
                     (274, SHORT, [1]), (279, LONG, [len(prev)]), (34665, LONG, [exif])], ifd1)
    t.set_first(ifd0)
    open("cr2_lj92_slices.cr2", "wb").write(t.bytes())
    try:
        libraw_check("cr2_lj92_slices.cr2", raw, None)
    except Exception as exc:
        print("  LibRaw could not verify the synthetic CR2:", exc)


def arw2():
    """Sony ARW2 structure: IFD0 preview JPEG, SubIFD with 64x16 raw data
    compressed as ARW2 blocks (compression 32767, 8 bits per pixel), tone
    curve tag 0x7010."""
    W, H = 64, 24
    knots = [8000, 10400, 12800, 14400]  # tag values (units of 4 in the curve's 12-bit domain)
    k = [0] + [(v >> 2) & 0xFFF for v in knots] + [4095]
    curve = [0] * 4096
    value = 0
    for j in range(1, 4096):
        seg = 0
        while seg < 4 and j > k[seg + 1]:
            seg += 1
        value += 1 << seg
        curve[j] = value
    out_of = [curve[c << 1] for c in range(2048)]
    black, white = 512, 16383
    cam = to_camera(scene(W, H))
    target = quantise(mosaic(cam, RGGB), black, white).astype(np.int64)
    # Code values: nearest curve output.
    table = np.array(out_of)
    codes = np.abs(table[None, None, :] - target[:, :, None]).argmin(axis=2)
    data = bytearray()
    decoded = np.zeros((H, W), np.uint16)
    for y in range(H):
        for span in range(0, W, 32):
            for parity in (0, 1):
                xs = [span + parity + 2 * i for i in range(16)]
                vals = [int(codes[y, x]) for x in xs]
                mx, mn = max(vals), min(vals)
                imax = vals.index(mx)
                imin = vals.index(mn)
                if imin == imax:
                    imin = (imax + 1) % 16
                    mn = vals[imin]
                shift = 0
                while shift < 4 and (0x80 << shift) <= mx - mn:
                    shift += 1
                bits = mx | (mn << 11) | (imax << 22) | (imin << 26)
                at = 30
                dec = []
                for i, v in enumerate(vals):
                    if i == imax:
                        dec.append(mx)
                    elif i == imin:
                        dec.append(mn)
                    else:
                        d = min((v - mn) >> shift, 127)
                        bits |= d << at
                        at += 7
                        dec.append(min((d << shift) + mn, 0x7FF))
                data += bits.to_bytes(16, "little")
                for i, x in enumerate(xs):
                    decoded[y, x] = out_of[dec[i]]
    t = Tiff()
    prev = jpeg(40, 10)
    p_off = t.blob(prev)
    r_off = t.blob(bytes(data))
    sub, _ = t.ifd([(254, LONG, [0]), (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [8]), (259, SHORT, [32767]),
                    (262, SHORT, [32803]), (273, LONG, [r_off]), (277, SHORT, [1]), (278, LONG, [H]),
                    (279, LONG, [len(data)]), (0x828D, SHORT, [2, 2]), (0x828E, BYTE, bytes([0, 1, 1, 2])),
                    (0x7010, SHORT, knots)])
    ifd0, _ = t.ifd([(254, LONG, [1]), (259, SHORT, [6]), (271, ASCII, "SONY"), (272, ASCII, "ILCE-Synthetic"),
                     (274, SHORT, [1]), (330, LONG, [sub]), (513, LONG, [p_off]), (514, LONG, [len(prev)])])
    t.set_first(ifd0)
    open("arw2_blocks.arw", "wb").write(t.bytes())
    try:
        libraw_check("arw2_blocks.arw", decoded, None)
    except Exception as exc:
        print("  LibRaw could not verify the synthetic ARW:", exc)


def nef_uncompressed():
    """Nikon NEF structure: IFD0 thumbnail + SubIFD raw (14-bit in 16-bit
    containers, RGGB) + maker note (white balance, black level, preview IFD)."""
    W, H = 48, 32
    black = 600
    white = 16383  # full 14-bit range (no white level recorded)
    cam = to_camera(scene(W, H))
    raw = quantise(mosaic(cam, RGGB), black, white)
    gains = (1.0 / NEUTRAL) / (1.0 / NEUTRAL).min()
    t = Tiff()
    thumb = jpeg(16, 10)
    th_off = t.blob(thumb)
    r_off = t.blob(u16(raw))
    # Maker note: "Nikon\0" 02 10 00 00 + embedded TIFF.
    mn = Tiff()
    prev = jpeg(40, 26)
    pv_off = mn.blob(prev)
    pv_ifd, _ = mn.ifd([(0x0201, LONG, [pv_off]), (0x0202, LONG, [len(prev)])])
    mn_ifd, _ = mn.ifd([(0x000C, RATIONAL, [gains[0], gains[2], 1.0, 1.0]), (0x003D, SHORT, [black] * 4),
                        (0x0011, LONG, [pv_ifd])])
    mn.set_first(mn_ifd)
    note = b"Nikon\x00\x02\x10\x00\x00" + mn.bytes()
    note_off = t.blob(note)
    exif, _ = t.ifd([(0x927C, "at", (UNDEFINED, len(note), note_off))])
    sub, _ = t.ifd([(254, LONG, [0]), (256, LONG, [W]), (257, LONG, [H]), (258, SHORT, [14]), (259, SHORT, [1]),
                    (262, SHORT, [32803]), (273, LONG, [r_off]), (277, SHORT, [1]), (278, LONG, [H]),
                    (279, LONG, [W * H * 2]), (0x828D, SHORT, [2, 2]), (0x828E, BYTE, bytes([0, 1, 1, 2]))])
    ifd0, _ = t.ifd([(254, LONG, [1]), (256, LONG, [16]), (257, LONG, [10]), (259, SHORT, [6]),
                     (271, ASCII, "NIKON CORPORATION"), (272, ASCII, "NIKON SYNTHETIC"), (274, SHORT, [1]),
                     (330, LONG, [sub]), (513, LONG, [th_off]), (514, LONG, [len(thumb)]), (34665, LONG, [exif])])
    t.set_first(ifd0)
    open("nef_uncompressed.nef", "wb").write(t.bytes())
    try:
        libraw_check("nef_uncompressed.nef", raw, None)
    except Exception as exc:
        print("  LibRaw could not verify the synthetic NEF:", exc)


def preview_containers():
    """Minimal ORF, RW2, RAF, CR3 and CRW containers whose only picture is a
    JPEG preview (their sensor data is not decoded)."""
    prev = jpeg(40, 24, color=(30, 160, 90))
    # ORF: "IIRO" header, IFD0 with a compressed-looking raw strip, Olympus
    # maker note with CameraSettings -> PreviewImageStart / Length.
    t = Tiff(header=b"IIRO\x08\x00\x00\x00")
    strip = t.blob(bytes(range(256)) * 4)
    mn = Tiff(header=b"OLYMPUS\x00II\x03\x00")
    mni, _ = mn.ifd([(0x2020, LONG, [30])])  # the main IFD sits right after the 12-byte header
    cs, _ = mn.ifd([(0x0101, LONG, [60]), (0x0102, LONG, [len(prev)])])
    pv = mn.blob(prev)
    assert (mni, cs, pv) == (12, 30, 60)
    note = mn.bytes()
    note_off = t.blob(note)
    exif, _ = t.ifd([(0x927C, "at", (UNDEFINED, len(note), note_off))])
    ifd0, _ = t.ifd([(256, LONG, [64]), (257, LONG, [48]), (258, SHORT, [12]), (259, SHORT, [1]),
                     (271, ASCII, "OLYMPUS"), (273, LONG, [strip]), (277, SHORT, [1]), (279, LONG, [1024]),
                     (274, SHORT, [8]), (34665, LONG, [exif])])
    t.set_first(ifd0)
    open("orf_preview.orf", "wb").write(t.bytes())
    # RW2: "IIU\0" header, IFD0 with the JPEG in tag 0x2E.
    t = Tiff(header=b"IIU\x00\x08\x00\x00\x00")
    ifd0, _ = t.ifd([(0x0002, SHORT, [64]), (0x0003, SHORT, [48]), (271, ASCII, "Panasonic"), (274, SHORT, [1]),
                     (0x002E, UNDEFINED, prev)])
    t.set_first(ifd0)
    open("rw2_preview.rw2", "wb").write(t.bytes())
    # RAF: fixed header with big-endian offsets.
    head = bytearray(b"FUJIFILMCCD-RAW 0201FF000000" + b"SYNTHETIC".ljust(32, b"\x00") + b"0100" + bytes(20))
    head += struct.pack(">IIIIII", 160, len(prev), 0, 0, 0, 0)
    head = head.ljust(160, b"\x00")
    open("raf_preview.raf", "wb").write(bytes(head) + prev)
    # CR3: ftyp crx + moov/trak with the JPEG as first sample + mdat.
    def box(kind, payload):
        return struct.pack(">I", 8 + len(payload)) + kind + payload
    ftyp = box(b"ftyp", b"crx " + struct.pack(">I", 1) + b"crx isom")

    def moov(offset):
        stsz = box(b"stsz", struct.pack(">III", 0, 0, 1) + struct.pack(">I", len(prev)))
        stco = box(b"stco", struct.pack(">II", 0, 1) + struct.pack(">I", offset))
        stbl = box(b"stbl", stsz + stco)
        return box(b"moov", box(b"trak", box(b"mdia", box(b"minf", stbl))))
    m = moov(0)
    offset = len(ftyp) + len(m) + 8
    m = moov(offset)
    open("cr3_preview.cr3", "wb").write(ftyp + m + box(b"mdat", prev))
    # CRW: CIFF heap with a JpgFromRaw (0x2007) record.
    header = b"II" + struct.pack("<I", 26) + b"HEAPCCDR" + struct.pack("<II", 0x10002, 0) + b"\x00" * 4
    heap = bytearray(prev)
    table = len(heap)
    heap += struct.pack("<H", 1) + struct.pack("<HII", 0x2007, len(prev), 0)
    heap += struct.pack("<I", table)
    open("crw_preview.crw", "wb").write(header + bytes(heap))


def main():
    dng_uncompressed16()
    dng_lj92_tiles()
    dng_packed12_orientation()
    dng_linear_float_deflate()
    dng_linear_lossy()
    dng_xtrans()
    dng_gainmap()
    cr2()
    arw2()
    nef_uncompressed()
    preview_containers()
    return 0


if __name__ == "__main__":
    sys.exit(main())
