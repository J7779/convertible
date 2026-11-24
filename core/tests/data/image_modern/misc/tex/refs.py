"""Decode the level data of Compressonator's KTX files with texture2ddecoder
(reference PNGs) and report which block modes the files exercise."""
import collections
import os
import struct

import texture2ddecoder as t2d
from PIL import Image

os.chdir(os.path.dirname(os.path.abspath(__file__)))


def level(path):
    d = open(path, 'rb').read()
    fields = struct.unpack('<13I', d[12:64])
    w, h, kv = fields[6], fields[7], fields[12]
    off = 64 + kv
    size = struct.unpack('<I', d[off:off + 4])[0]
    return fields[3], w, h, d[off + 4:off + 4 + size]


dec = {'bc7_noise': t2d.decode_bc7, 'bc7_noise_rgb': t2d.decode_bc7, 'etc2a1_noise': t2d.decode_etc2a1,
       'etc2_noise': t2d.decode_etc2, 'bc1_noise': t2d.decode_bc1}
for name, fn in dec.items():
    fmt, w, h, data = level(name + '.ktx')
    Image.frombytes('RGBA', (w, h), fn(data, w, h), 'raw', 'BGRA').save(name + '_ref.png')
    print(name, hex(fmt), w, h, len(data))
    if name.startswith('bc7'):
        print(' modes', dict(collections.Counter(next((k for k in range(8) if data[i] >> k & 1), 8)
                                                 for i in range(0, len(data), 16))))
    if name.startswith('etc2'):
        c = collections.Counter()
        punch = name == 'etc2a1_noise'
        for i in range(0, len(data), 8):
            v = int.from_bytes(data[i:i + 8], 'big')

            def s3(x):
                return x - 8 if x >= 4 else x
            if not punch and not (v >> 33) & 1:
                c['ind'] += 1
                continue
            r, g, b = (v >> 59) & 31, (v >> 51) & 31, (v >> 43) & 31
            if not 0 <= r + s3((v >> 56) & 7) <= 31:
                c['T'] += 1
            elif not 0 <= g + s3((v >> 48) & 7) <= 31:
                c['H'] += 1
            elif not 0 <= b + s3((v >> 40) & 7) <= 31:
                c['P'] += 1
            else:
                c['diff'] += 1
            if punch and not (v >> 33) & 1:
                c['non-opaque'] += 1
        print(' ', dict(c))
