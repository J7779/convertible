#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
"""Remove machine data from the HFS+ volume of a raw (UDRW) disk image, in place.

    python3 hfs_scrub.py image.dmg

hdiutil copies the creator's uid/gid, stamps the volume, the root folder, the
private folders and every attribute-modification date with the current time,
records a "date added" in the Finder info, and copies the com.apple.provenance
extended attribute. This script, run before `hdiutil convert`:
  * sets owner and group of every catalog record to 0 (hard-link records keep
    those fields: there they hold the link chain, not ids),
  * replaces every date within the last 30 days with 2021-06-15T12:00:00Z, in
    the primary and alternate volume headers and in every catalog record,
  * clears a recent "date added" in the extended Finder info,
  * zeroes the value of every com.apple.provenance attribute.
File contents, modes, names and the B-tree structure are not touched.
"""
import struct
import sys
import time

HFS_EPOCH = 2082844800
FIXED = 1623758400 + HFS_EPOCH
data = bytearray(open(sys.argv[1], 'rb').read())
now = int(time.time()) + HFS_EPOCH


def recent(v):
    return now - 30 * 86400 < v < now + 86400


start = None
for off in range(0, len(data) - 2048, 512):
    if data[off + 1024:off + 1026] in (b'H+', b'HX') and struct.unpack('>H', data[off + 1026:off + 1028])[0] in (4, 5):
        start = off
        break
assert start is not None, 'no HFS+ volume header'
vh = start + 1024
block_size, total_blocks = struct.unpack('>II', data[vh + 40:vh + 48])
for header in (vh, start + total_blocks * block_size - 1024):
    if data[header:header + 2] in (b'H+', b'HX'):
        for i in range(4):
            at = header + 16 + 4 * i
            if recent(struct.unpack('>I', data[at:at + 4])[0]):
                struct.pack_into('>I', data, at, FIXED)


def leaf_records(fork_at):
    first_block = struct.unpack('>I', data[fork_at + 16:fork_at + 20])[0]
    base = start + first_block * block_size  # the small volumes made here have one extent
    node_size = struct.unpack('>H', data[base + 32:base + 34])[0]
    total = struct.unpack('>I', data[base + 36:base + 40])[0]
    for n in range(total):
        node = base + n * node_size
        if struct.unpack('>b', data[node + 8:node + 9])[0] != -1:
            continue
        for r in range(struct.unpack('>H', data[node + 10:node + 12])[0]):
            yield node + struct.unpack('>H', data[node + node_size - 2 * (r + 1):node + node_size - 2 * r])[0]


for at in leaf_records(vh + 272):  # catalog
    rec = at + 2 + struct.unpack('>H', data[at:at + 2])[0]
    if struct.unpack('>H', data[rec:rec + 2])[0] not in (1, 2):
        continue
    if bytes(data[rec + 48:rec + 52]) != b'hlnk':
        struct.pack_into('>II', data, rec + 32, 0, 0)
    for i in range(5):
        if recent(struct.unpack('>I', data[rec + 12 + 4 * i:rec + 16 + 4 * i])[0]):
            struct.pack_into('>I', data, rec + 12 + 4 * i, FIXED)
    if recent(struct.unpack('>I', data[rec + 68:rec + 72])[0] + HFS_EPOCH):
        struct.pack_into('>I', data, rec + 68, 0)

if struct.unpack('>Q', data[vh + 352:vh + 360])[0]:
    for at in leaf_records(vh + 352):  # attributes
        key_len = struct.unpack('>H', data[at:at + 2])[0]
        name_len = struct.unpack('>H', data[at + 12:at + 14])[0]
        name = data[at + 14:at + 14 + 2 * name_len].decode('utf-16-be')
        rec = at + 2 + key_len
        kind, _, _, size = struct.unpack('>IIII', data[rec:rec + 16])
        if kind == 0x10 and name == 'com.apple.provenance':
            data[rec + 16:rec + 16 + size] = bytes(size)

open(sys.argv[1], 'wb').write(data)
