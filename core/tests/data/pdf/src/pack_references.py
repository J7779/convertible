#!/usr/bin/env python3
# Runs inside the fixture container: packs reference renders and extracts raw fax strips.
import os, struct, zlib, glob

W = '/w'
OUT = W + '/out'
WORK = W + '/work'
os.makedirs(OUT + '/ref', exist_ok=True)

def read_pgm(path):
    data = open(path, 'rb').read()
    # P5 header with optional comments
    parts = []
    i = 2
    while len(parts) < 3:
        while data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b'#':
            while data[i:i+1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j+1].isspace():
            j += 1
        parts.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = parts
    return w, h, data[i:i + w * h]

for name in ['gs_graphics', 'latex_article', 'libreoffice_report', 'gs_type3_base14', 'weasyprint_report',
             'gs_cff_article', 'img2pdf_png', 'latex_twocolumn', 'ccitt_g4']:
    path = f'{WORK}/ref_{name}_1.pgm'
    if not os.path.exists(path):
        continue
    w, h, px = read_pgm(path)
    blob = struct.pack('<II', w, h) + px
    open(f'{OUT}/ref/{name}.p1.gray.zz', 'wb').write(zlib.compress(blob, 9))

for name in ['latex_article', 'libreoffice_report', 'weasyprint_report', 'latex_twocolumn', 'gs_cff_article',
             'gs_type3_base14', 'latex_small']:
    src = f'{WORK}/ref_{name}.txt'
    if os.path.exists(src):
        open(f'{OUT}/ref/{name}.pdftotext.txt', 'wb').write(open(src, 'rb').read())

# Raw CCITT strips from the libtiff files (single strip each).
def tiff_strip(path):
    d = open(path, 'rb').read()
    le = d[:2] == b'II'
    e = '<' if le else '>'
    off = struct.unpack(e + 'I', d[4:8])[0]
    n = struct.unpack(e + 'H', d[off:off+2])[0]
    tags = {}
    for k in range(n):
        ent = d[off + 2 + 12 * k: off + 14 + 12 * k]
        tag, typ, cnt = struct.unpack(e + 'HHI', ent[:8])
        if typ == 3 and cnt == 1:
            val = struct.unpack(e + 'H', ent[8:10])[0]
        else:
            val = struct.unpack(e + 'I', ent[8:12])[0]
        tags[tag] = val
    start, length = tags[273], tags[279]
    return d[start:start + length], tags.get(292, 0)

for src, dst in [('fax_g3_1d.tif', 'fax_g3_1d.raw'), ('fax_g3_2d.tif', 'fax_g3_2d.raw'), ('fax_g4.tif', 'fax_g4.raw')]:
    strip, opts = tiff_strip(f'{WORK}/{src}')
    open(f'{OUT}/{dst}', 'wb').write(strip)
    print(dst, len(strip), 'T4Options', opts)
