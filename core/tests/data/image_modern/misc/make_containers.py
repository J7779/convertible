#!/usr/bin/env python3
"""Assemble the ORA, KRA and ANI fixtures from pictures made by ImageMagick.

Usage: python3 make_containers.py <source dir> <output dir>
Only the Python standard library is used (zipfile, struct).
"""
import os
import struct
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]


def read(name):
    with open(os.path.join(src, name), "rb") as f:
        return f.read()


def package(path, mimetype, entries):
    with zipfile.ZipFile(path, "w") as z:
        info = zipfile.ZipInfo("mimetype", date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        z.writestr(info, mimetype)
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)


stack = """<?xml version='1.0' encoding='UTF-8'?>
<image version="0.0.5" w="48" h="32" xres="72" yres="72">
 <stack>
  <layer name="hidden" src="data/hidden.png" visibility="hidden" x="0" y="0"/>
  <stack name="group" x="20" y="10">
   <layer name="dot" src="data/dot.png" x="10" y="4" opacity="1.0"/>
  </stack>
  <layer name="box" src="data/box.png" x="10" y="8" opacity="0.5" composite-op="svg:src-over"/>
  <layer name="background" src="data/background.png" x="0" y="0" opacity="1.0"/>
 </stack>
</image>
"""
layers = [("stack.xml", stack.encode()), ("data/background.png", read("l0.png")), ("data/box.png", read("l1.png")),
          ("data/dot.png", read("l2.png")), ("data/hidden.png", read("l3.png"))]
# Without mergedimage.png: the reader must composite the stack.
package(os.path.join(out, "layers_nomerged.ora"), "image/openraster", layers)
# With a merged image (deliberately a different picture) that must win.
package(os.path.join(out, "merged.ora"), "image/openraster",
        layers + [("mergedimage.png", read("kra_merged.png")), ("Thumbnails/thumbnail.png", read("kra_preview.png"))])

maindoc = b"<?xml version='1.0' encoding='UTF-8'?>\n<DOC syntaxVersion='2'><IMAGE width='24' height='16'/></DOC>\n"
package(os.path.join(out, "merged.kra"), "application/x-krita",
        [("maindoc.xml", maindoc), ("mergedimage.png", read("kra_merged.png")), ("preview.png", read("kra_preview.png"))])
package(os.path.join(out, "preview_only.kra"), "application/x-krita",
        [("maindoc.xml", maindoc), ("preview.png", read("kra_preview.png"))])


def chunk(cid, data):
    pad = b"\0" if len(data) % 2 else b""
    return cid + struct.pack("<I", len(data)) + data + pad


def ani(path, frames, steps, rates, seq, default_rate):
    flags = 1 | (2 if seq is not None else 0)
    anih = struct.pack("<9I", 36, len(frames), steps, 0, 0, 0, 0, default_rate, flags)
    body = b"ACON"
    body += chunk(b"LIST", b"INFO" + chunk(b"INAM", b"Test pattern\0"))
    body += chunk(b"anih", anih)
    if rates is not None:
        body += chunk(b"rate", b"".join(struct.pack("<I", r) for r in rates))
    if seq is not None:
        body += chunk(b"seq ", b"".join(struct.pack("<I", s) for s in seq))
    body += chunk(b"LIST", b"fram" + b"".join(chunk(b"icon", f) for f in frames))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


# Cursor frames, a 5-step sequence with per-step rates.
ani(os.path.join(out, "cursor_seq.ani"), [read("f0.cur"), read("f1.cur"), read("f2.cur")], 5, [6, 12, 30, 6, 3],
    [0, 1, 2, 1, 0], 10)
# Icon frames, no sequence, default rate only.
ani(os.path.join(out, "icons.ani"), [read("f0.ico"), read("f1.ico"), read("f2.ico")], 3, None, None, 15)
