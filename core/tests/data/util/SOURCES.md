# Sources: util

Streams and archives written by independent tools, decoded by the tests of
the in-house DEFLATE / zlib / gzip code (`core/tests/util/deflate_test.cpp`)
and of the ZIP container (`core/tests/util/zip_test.cpp`).

This file covers the files directly in this directory; sub-directories carry
their own `SOURCES.md`.

All content is synthetic and is regenerated inside the tests, which compare
the decoded bytes with it:

- `LOREM`: the 446-byte placeholder paragraph below, ending in one LF.
- `pattern(n)`: `n` bytes, byte `i` = `(i % 251) ^ ((i / 251) & 15)`.

Tools: Python 3.14.10 (standard library only; its `zlib` module is linked
against zlib 1.2.12), Apple gzip 487.40.2, Info-ZIP Zip 3.0. Commands are run
in this directory. Time stamps are fixed (2001-02-03 04:05:06 UTC =
981173106, or absent) and no file carries user, host or path information.

## Step 1: Python-made files and the inputs of the command-line tools

Save as `make_fixtures.py`, run `python3 make_fixtures.py`:

```python
import io, zipfile, zlib

LOREM = (b"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
         b"dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
         b"commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat "
         b"nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit "
         b"anim id est laborum.\n")

def pattern(n):
    return bytes(((i % 251) ^ ((i // 251) & 15)) for i in range(n))

# input files for the command-line tools (not committed)
open("lorem.txt", "wb").write(LOREM * 2)
open("pattern.bin", "wb").write(pattern(600))

# zlib stream, level 9
open("lorem_x4_level9.zlib", "wb").write(zlib.compress(LOREM * 4, 9))

# raw DEFLATE, fixed Huffman codes only
c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_FIXED)
open("pattern_4000_fixed.deflate", "wb").write(c.compress(pattern(4000)) + c.flush())

# raw DEFLATE, one dynamic Huffman block
c = zlib.compressobj(6, zlib.DEFLATED, -15)
open("lorem_x8_dynamic.deflate", "wb").write(c.compress(LOREM * 8) + c.flush())

# raw DEFLATE, level 6 with flush points: compressed blocks separated by empty stored blocks
c = zlib.compressobj(6, zlib.DEFLATED, -15)
s = c.compress(LOREM) + c.flush(zlib.Z_SYNC_FLUSH)
s += c.compress(pattern(1000)) + c.flush(zlib.Z_FULL_FLUSH)
s += c.compress(LOREM) + c.flush()
open("lorem_pattern_lorem_flushed.deflate", "wb").write(s)

# raw DEFLATE, level 0: stored blocks, with a flush (empty stored block) in between
c = zlib.compressobj(0, zlib.DEFLATED, -15)
s = c.compress(LOREM) + c.flush(zlib.Z_SYNC_FLUSH) + c.compress(pattern(300)) + c.flush()
open("lorem_pattern_stored.deflate", "wb").write(s)

class Unseekable(io.RawIOBase):
    """A write-only stream without seek/tell: zipfile then writes data descriptors."""
    def __init__(self, f): self.f = f
    def writable(self): return True
    def write(self, b): return self.f.write(b)

# ZIP with data descriptors (general-purpose bit 3)
with open("data_descriptors.zip", "wb") as f:
    with zipfile.ZipFile(Unseekable(f), "w") as z:
        z.writestr(zipfile.ZipInfo("lorem.txt", date_time=(2001, 2, 3, 4, 5, 6)), LOREM * 3,
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(zipfile.ZipInfo("sub/pattern.bin", date_time=(2001, 2, 3, 4, 5, 8)), pattern(500),
                   compress_type=zipfile.ZIP_STORED)

# ZIP whose entries carry ZIP64 extra fields although they are small
with zipfile.ZipFile("forced_zip64.zip", "w") as z:
    info = zipfile.ZipInfo("lorem.txt", date_time=(2001, 2, 3, 4, 5, 6))
    info.compress_type = zipfile.ZIP_DEFLATED
    with z.open(info, "w", force_zip64=True) as f:
        f.write(LOREM * 3)
    info = zipfile.ZipInfo("pattern.bin", date_time=(2001, 2, 3, 4, 5, 8))
    info.compress_type = zipfile.ZIP_STORED
    with z.open(info, "w", force_zip64=True) as f:
        f.write(pattern(500))
```

## Step 2: gzip and zip

```sh
export TZ=UTC
touch -t 200102030405.06 lorem.txt
gzip -9 -c lorem.txt > lorem_x2_named.gz
(printf 'first member, ' | gzip -n -c; printf 'second member\n' | gzip -n -9 -c) > two_members.gz
mkdir -p tree/dir
cp lorem.txt tree/dir/lorem.txt
cp pattern.bin tree/pattern.bin
touch -t 200102030405.06 tree/dir/lorem.txt tree/pattern.bin tree/dir
(cd tree && zip -X -0 -q ../zip_stored_deflated_dir.zip pattern.bin && zip -X -9 -q ../zip_stored_deflated_dir.zip dir/ dir/lorem.txt)
```

Afterwards delete `make_fixtures.py`, `lorem.txt`, `pattern.bin` and `tree/`.

## Files

| File | Contents | Tool | Made by |
| --- | --- | --- | --- |
| `lorem_x4_level9.zlib` | zlib stream (header `78 DA`) of `LOREM` x 4 (1784 bytes) | Python `zlib.compress`, level 9 | step 1 |
| `pattern_4000_fixed.deflate` | raw DEFLATE of `pattern(4000)`: fixed-Huffman blocks only (`Z_FIXED`), literals and matches | Python `zlib.compressobj` | step 1 |
| `lorem_x8_dynamic.deflate` | raw DEFLATE of `LOREM` x 8 (3568 bytes): one dynamic-Huffman block | Python `zlib.compressobj`, level 6 | step 1 |
| `lorem_pattern_lorem_flushed.deflate` | raw DEFLATE of `LOREM` + `pattern(1000)` + `LOREM` (1892 bytes) with a sync flush and a full flush between the parts: several blocks, two empty stored blocks | Python `zlib.compressobj`, level 6 | step 1 |
| `lorem_pattern_stored.deflate` | raw DEFLATE of `LOREM` + `pattern(300)` (746 bytes) at level 0: stored blocks, one of them empty | Python `zlib.compressobj`, level 0 | step 1 |
| `lorem_x2_named.gz` | gzip member of `LOREM` x 2 (892 bytes) with the stored name `lorem.txt` and mtime 981173106 | Apple gzip 487.40.2 | step 2 |
| `two_members.gz` | two concatenated gzip members without name or time: `first member, ` and `second member` + LF | Apple gzip 487.40.2 | step 2 |
| `zip_stored_deflated_dir.zip` | `pattern.bin` (stored, `pattern(600)`), `dir/` (directory), `dir/lorem.txt` (deflated, `LOREM` x 2); all dated 2001-02-03 04:05:06, modes 0644 / 0755, no extra fields | Info-ZIP Zip 3.0 | step 2 |
| `data_descriptors.zip` | `lorem.txt` (deflated, `LOREM` x 3) and `sub/pattern.bin` (stored, `pattern(500)`), written to a non-seekable stream: general-purpose bit 3 set, sizes and CRC zero in the local headers, data descriptors with signature after the data; dated 2001-02-03 04:05:06 and 04:05:08 | Python `zipfile` | step 1 |
| `forced_zip64.zip` | the same two entries (`lorem.txt`, `pattern.bin`) written with `force_zip64=True`: local headers need version 4.5, hold `FFFFFFFF` sizes and a ZIP64 extra field | Python `zipfile` | step 1 |

`.gitattributes` (`* -text`) keeps git from translating these files.
