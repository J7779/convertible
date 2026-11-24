#!/usr/bin/env python3
# Wraps codestreams written by opj_compress (make_opj.sh) into JP2/JPX files
# with boxes that opj_compress cannot write itself: a palette (pclr + cmap), an
# sYCC colour specification, a restricted ICC profile with a resolution box,
# and a JPX file type with a reader requirements box. The boxes follow
# ISO/IEC 15444-1 Annex I and 15444-2 Annex M. Standard library only; run from
# this directory: python3 -I make_wrapped.py
import struct


def box(kind, body):
    return struct.pack(">I", 8 + len(body)) + kind + body


SIGNATURE = box(b"jP  ", b"\r\n\x87\n")


def ftyp(brand, compat):
    return box(b"ftyp", brand + struct.pack(">I", 0) + b"".join(compat))


def ihdr(width, height, nc, bpc):
    return box(b"ihdr", struct.pack(">IIHBBBB", height, width, nc, bpc, 7, 0, 0))


def colr_enum(enumcs):
    return box(b"colr", struct.pack(">BBBI", 1, 0, 0, enumcs))


def siz(codestream):
    # SOC, then SIZ: Lsiz Rsiz Xsiz Ysiz XOsiz YOsiz ... Csiz
    assert codestream[0:4] == b"\xff\x4f\xff\x51"
    xsiz, ysiz, xo, yo = struct.unpack(">IIII", codestream[8:24])
    csiz = struct.unpack(">H", codestream[40:42])[0]
    ssiz = codestream[42]
    return xsiz - xo, ysiz - yo, csiz, ssiz


def write(path, parts):
    with open(path, "wb") as f:
        f.write(b"".join(parts))


def main():
    # Palette: 16 indexed colours, three 8-bit columns.
    index = open("index.j2k", "rb").read()
    w, h, nc, ssiz = siz(index)
    entries = []
    for i in range(16):
        entries.append(((i * 53) % 256, (i * 97 + 40) % 256, (255 - i * 16) % 256))
    pclr = box(b"pclr", struct.pack(">HB", 16, 3) + bytes([7, 7, 7]) + b"".join(bytes(e) for e in entries))
    cmap = box(b"cmap", b"".join(struct.pack(">HBB", 0, 1, col) for col in range(3)))
    jp2h = box(b"jp2h", ihdr(w, h, 1, ssiz) + colr_enum(16) + pclr + cmap)
    write("palette.jp2", [SIGNATURE, ftyp(b"jp2 ", [b"jp2 "]), jp2h, box(b"jp2c", index)])

    # sYCC 4:2:0 in a JP2 file (colour space enumeration 18).
    ycc = open("ycc420.j2k", "rb").read()
    w, h, nc, ssiz = siz(ycc)
    jp2h = box(b"jp2h", ihdr(w, h, nc, ssiz) + colr_enum(18))
    write("sycc420.jp2", [SIGNATURE, ftyp(b"jp2 ", [b"jp2 "]), jp2h, box(b"jp2c", ycc)])

    # Restricted ICC profile (a synthetic 132-byte display profile with an
    # empty tag table) and a capture resolution of 300 x 150 dpi.
    rgb = open("tiles_layers_rpcl.j2k", "rb").read()
    w, h, nc, ssiz = siz(rgb)
    icc = bytearray(132)
    struct.pack_into(">I", icc, 0, 132)
    icc[4:8] = b"none"
    struct.pack_into(">I", icc, 8, 0x02100000)
    icc[12:16] = b"mntr"
    icc[16:20] = b"RGB "
    icc[20:24] = b"XYZ "
    icc[36:40] = b"acsp"
    struct.pack_into(">iii", icc, 68, 63190, 65536, 54061)  # D50 illuminant
    struct.pack_into(">I", icc, 128, 0)  # tag count
    colr_icc = box(b"colr", struct.pack(">BBB", 2, 0, 0) + bytes(icc))

    def resolution(dpi):
        # pixels per metre as a fraction num/den with exponent 0
        ppm = round(dpi / 0.0254)
        return ppm, 1

    vn, vd = resolution(150)
    hn, hd = resolution(300)
    resc = box(b"resc", struct.pack(">HHHHbb", vn, vd, hn, hd, 0, 0))
    res = box(b"res ", resc)
    jp2h = box(b"jp2h", ihdr(w, h, nc, ssiz) + colr_icc + res)
    write("icc_res.jp2", [SIGNATURE, ftyp(b"jp2 ", [b"jp2 "]), jp2h, box(b"jp2c", rgb)])

    # JPX: brand 'jpx ', JP2-compatible, reader requirements: feature 5
    # (unrestricted Part 1 codestream), fully understood and displayable.
    lossless = open("rgb_lossless.jp2", "rb").read()
    at = lossless.find(b"jp2c") - 4
    codestream = lossless[at + 8:]
    w, h, nc, ssiz = siz(codestream)
    rreq = box(b"rreq", struct.pack(">BBBHHBH", 1, 0x80, 0x80, 1, 5, 0x80, 0))
    jp2h = box(b"jp2h", ihdr(w, h, nc, ssiz) + colr_enum(16))
    write("rgb.jpx", [SIGNATURE, ftyp(b"jpx ", [b"jpx ", b"jp2 ", b"jpxb"]), rreq, jp2h, box(b"jp2c", codestream)])


if __name__ == "__main__":
    main()
