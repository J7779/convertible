# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
"""Normalise the creation/change times bsdtar writes into a xar TOC (they come from the clock); SHA-1 or MD5 TOC checksums."""
import hashlib
import re
import struct
import sys
import zlib

path = sys.argv[1]
data = open(path, 'rb').read()
magic, header_size, version, clen, ulen, cksum_alg = struct.unpack('>4sHHQQI', data[:28])
toc = zlib.decompress(data[header_size:header_size + clen]).decode('utf-8')
heap = data[header_size + clen:]
toc = re.sub(r'<creation-time>[^<]*</creation-time>', '<creation-time>2021-06-15T12:00:00</creation-time>', toc)
toc = re.sub(r'<ctime>[^<]*</ctime>', '<ctime>2021-06-15T12:00:00Z</ctime>', toc)
raw = toc.encode('utf-8')
packed = zlib.compress(raw, 9)
assert cksum_alg in (1, 2)
sum_ = (hashlib.sha1 if cksum_alg == 1 else hashlib.md5)(packed).digest()
heap = sum_ + heap[len(sum_):]
header = struct.pack('>4sHHQQI', magic, header_size, version, len(packed), len(raw), cksum_alg) + data[28:header_size]
open(path, 'wb').write(header + packed + heap)
