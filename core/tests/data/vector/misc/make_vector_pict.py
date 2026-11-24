# Hand-written PICT files per "Inside Macintosh: Imaging With QuickDraw", appendix A.
import struct, sys

def w(*v):  # big-endian 16-bit words
    return b"".join(struct.pack(">h" if x < 0 else ">H", x) for x in v)

def rect(t, l, b, r):
    return w(t, l, b, r)

def rgb(r, g, b):
    return w(r, g, b)

def v2(ops):
    body = b""
    for op, data in ops:
        body += w(op) + data
        if len(body) % 2:
            body += b"\0"
    return body

frame = rect(0, 0, 100, 200)
header = w(0x0011, 0x02FF, 0x0C00) + w(0xFFFE, 0) + struct.pack(">II", 72 << 16, 72 << 16) + frame + w(0, 0)
ops = [
    (0x001E, b""),                                   # DefHilite
    (0x0001, w(10) + frame),                         # Clip region = frame
    (0x00A0, w(100)),                                # ShortComment
    (0x00A1, w(150, 4) + b"abcd"),                   # LongComment
    (0x001A, rgb(0xFFFF, 0, 0)),                     # RGBFgCol red
    (0x0031, rect(10, 10, 30, 50)),                  # paintRect
    (0x001A, rgb(0, 0, 0xFFFF)),                     # RGBFgCol blue
    (0x0007, w(4, 4)),                               # PnSize 4x4
    (0x0050, rect(10, 60, 40, 100)),                 # frameOval
    (0x001A, rgb(0, 0x8000, 0)),                     # green
    (0x0007, w(2, 2)),
    (0x0020, w(50, 10, 50, 100)),                    # Line (v,h) -> (v,h)
    (0x0022, w(60, 10) + struct.pack(">bb", 20, 5)), # ShortLine from (60,10) dh 20 dv 5
    (0x001A, rgb(0xFFFF, 0x8000, 0)),                # orange
    (0x0071, w(22) + rect(60, 120, 90, 160) + w(60, 140, 90, 160, 90, 120)),  # paintPoly triangle
    (0x001A, rgb(0x8000, 0, 0x8000)),                # purple
    (0x0061, rect(10, 120, 50, 160) + w(90, 90)),    # paintArc: start 90 (3 o'clock), 90 degrees clockwise
    (0x000B, w(10, 10)),                             # OvSize
    (0x001A, rgb(0, 0, 0)),
    (0x0007, w(1, 1)),
    (0x0040, rect(60, 170, 90, 195)),                # frameRRect
    (0x002C, w(12) + w(21) + bytes([9]) + b"Helvetica"),  # fontName
    (0x0003, w(21)),                                 # TxFont
    (0x000D, w(14)),                                 # TxSize
    (0x0004, bytes([1])),                            # TxFace bold
    (0x0028, w(95, 10) + bytes([5]) + b"Hello"),     # LongText at (v95, h10)
    (0x0029, bytes([40, 3]) + b"Caf" ),              # DHText dh 40
    (0x0023, struct.pack(">bb", 0, 0)),              # ShortLineFrom (no-op)
    (0x00FF, b""),
]
body = header + v2(ops)
pic = w(0) + frame + body          # picSize is ignored for v2 (low word only)
with open(sys.argv[1], "wb") as f:
    f.write(b"\0" * 512 + pic)

# Version 1: one-byte opcodes, no 512-byte header.
frame1 = rect(0, 0, 40, 60)
ops1 = bytes([0x11, 0x01, 0xA0]) + w(42) + bytes([0x01]) + w(10) + frame1
ops1 += bytes([0x09]) + bytes([0xAA, 0x55] * 4)    # PnPat 50% grey
ops1 += bytes([0x31]) + rect(5, 5, 35, 25)          # paintRect with the pen pattern
ops1 += bytes([0x0A]) + bytes([0xFF] * 8)           # FillPat black
ops1 += bytes([0x54]) + rect(5, 30, 35, 55)          # fillOval
ops1 += bytes([0x22]) + w(38, 0) + struct.pack(">bb", 60, 0)  # ShortLine
ops1 += bytes([0xFF])
pic1 = w(len(ops1) + 10) + frame1 + ops1
with open(sys.argv[2], "wb") as f:
    f.write(pic1)
