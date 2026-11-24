# ZIP fixtures

All files were written in the Docker image `cvt-archive-tools:1` (Debian 12,
linux/amd64), running as root with `TZ=UTC`, from the synthetic tree built by
`../make_tree.py` (see `../SOURCES.md`; every file has mtime 2021-06-15T12:00:00Z).
Tools: Info-ZIP Zip 3.0, 7-Zip 26 (`7zz`), Python 3.11 `zipfile`.
Commands run with `LANG=C.UTF-8 LC_ALL=C.UTF-8` from inside the tree (`cd t`)
unless noted; `-X` keeps Info-ZIP from storing uid/gid extras.

| File | Content | Command |
| --- | --- | --- |
| `tree_deflate.zip` | the tree without `long/`; `link` followed (a copy of hello.txt), deflate | `zip -X -q -r ../a/tree_deflate.zip . -x long/\*` |
| `tree_symlink.zip` | hello.txt, `link` stored as a symlink, `dir/` | `zip -X -q -r -y ../a/tree_symlink.zip hello.txt link dir` |
| `tree_store.zip` | hello.txt, lorem.txt, stored | `zip -X -q -0 ../a/tree_store.zip hello.txt lorem.txt` |
| `zipcrypto.zip` | hello.txt, lorem.txt, ZipCrypto, password `secret` | `zip -X -q -P secret ../a/zipcrypto.zip hello.txt lorem.txt` |
| `zipcrypto_stream.zip` | the same written to a pipe: data descriptors, the password check byte comes from the DOS time | `zip -X -q -P secret - hello.txt lorem.txt \| cat > ../a/zipcrypto_stream.zip` |
| `stream_descriptor.zip` | lorem.txt from stdin as entry `-`, written to a pipe: bit 3, Zip64 local extra, 64-bit data descriptor | `cat lorem.txt \| zip -X -q - - \| cat > ../a/stream_descriptor.zip`, then `python3 set_dos_time.py a/stream_descriptor.zip` (below) |
| `stream_dd32.zip` | lorem.txt, hello.txt written to a pipe: bit 3, 32-bit data descriptors with signature | `zip -X -q -fz- - lorem.txt hello.txt \| cat > ../a/stream_dd32.zip` |
| `zip64_forced.zip` | hello.txt, noise.bin with Zip64 extras and end records | `zip -X -q -fz ../a/zip64_forced.zip hello.txt noise.bin` |
| `infozip_extras.zip` | hello.txt and the unicode file with Info-ZIP's default `UT` and `ux` (uid/gid 0) extras; UTF-8 name without the UTF-8 flag | `zip -q ../a/infozip_extras.zip hello.txt "unicode/ünïcödé-名前.txt"` |
| `cp437_name.zip` | one file named `caf` 0x82 `.txt` (CP437 é), no UTF-8 flag, content `cafe\n` | in an empty directory `c`, `LANG=C LC_ALL=C`: `printf 'cafe\n' > "c/$(printf 'caf\202.txt')"; touch -d @1623758400 c/*; cd c && zip -X -q ../a/cp437_name.zip *` |
| `deflate64.zip` | lorem.txt (Deflate64), noise.bin (stored) | `7zz a -bd -tzip -mm=Deflate64 -mx=9 ../a/deflate64.zip lorem.txt noise.bin` |
| `bzip2.zip` | lorem.txt (bzip2), hello.txt (stored) | `7zz a -bd -tzip -mm=BZip2 ../a/bzip2.zip lorem.txt hello.txt` |
| `lzma.zip` | lorem.txt (LZMA with end marker), hello.txt (stored) | `7zz a -bd -tzip -mm=LZMA ../a/lzma.zip lorem.txt hello.txt` |
| `ppmd.zip` | lorem.txt (PPMd var.I), hello.txt (stored) | `7zz a -bd -tzip -mm=PPMd ../a/ppmd.zip lorem.txt hello.txt` |
| `aes256.zip` | lorem.txt, hello.txt, WinZip AES-256 (AE-2), password `secret` | `7zz a -bd -tzip -mem=AES256 -psecret ../a/aes256.zip lorem.txt hello.txt` |
| `aes128.zip` | hello.txt, WinZip AES-128 (AE-2), password `secret` | `7zz a -bd -tzip -mem=AES128 -psecret ../a/aes128.zip hello.txt` |
| `unicode.zip` | `unicode/` and the unicode file with the UTF-8 flag and NTFS timestamps | `7zz a -bd -tzip -mcu=on ../a/unicode.zip "unicode"` |
| `python_comment.zip` | archive comment, hello.txt and `café.txt` (UTF-8 flag), modes 0644 without file-type bits | Python script below (run in the parent of `t`) |
| `sfx.zip` | a shell-script stub followed by `tree_store.zip`, offsets adjusted | `printf '#!/bin/sh\necho "this is a stub"\nexit 0\n' > stub.sh; cat stub.sh a/tree_store.zip > a/sfx.zip && zip -q -A a/sfx.zip` |

`python_comment.zip`:

```python
import zipfile
with zipfile.ZipFile('a/python_comment.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.comment = b'synthetic archive comment'
    info = zipfile.ZipInfo('hello.txt', date_time=(2021, 6, 15, 12, 0, 0)); info.external_attr = 0o644 << 16
    z.writestr(info, open('t/hello.txt', 'rb').read())
    info = zipfile.ZipInfo('café.txt', date_time=(2021, 6, 15, 12, 0, 0)); info.external_attr = 0o644 << 16
    z.writestr(info, b'cafe\n')
```

`set_dos_time.py` (Info-ZIP stamps an entry read from stdin with the clock;
this replaces that DOS time/date by 2021-06-15 12:00:00 in the local and the
central header; nothing else changes):

```python
import struct, sys
TIME, DATE = 0x6000, 0x52CF
path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
eocd = data.rfind(b'PK\x05\x06')
count, cd_size, cd_offset = struct.unpack_from('<HII', data, eocd + 10)
pos = cd_offset
for _ in range(count):
    struct.pack_into('<HH', data, pos + 12, TIME, DATE)
    name_len, extra_len, comment_len = struct.unpack_from('<HHH', data, pos + 28)
    local = struct.unpack_from('<I', data, pos + 42)[0]
    struct.pack_into('<HH', data, local + 10, TIME, DATE)
    pos += 46 + name_len + extra_len + comment_len
open(path, 'wb').write(data)
```

Other cases (Info-ZIP Unicode Path extra 0x7075, data descriptors without
signature, MS-DOS hosts, split-archive markers, overlapping entries, CRX2/CRX3
headers, lying sizes) are built byte by byte inside `format_zip_test.cpp`.

## Interop of our output

The writer's output for the tree (`convertible convert tree_pax.tar -t zip`)
with every `method` (deflate, store, bzip2, lzma, zstd, xz) and every
`encryption` (none, aes256, aes128, zipcrypto; password `secret`) - 24
archives - was checked in `cvt-archive-tools:1`:

| Tool | Result |
| --- | --- |
| 7-Zip 26 (`7zz t -psecret`, `7zz x -snl`) | all 24 test OK; extraction byte-identical to the tree (`diff -r --no-dereference`: content, symlink, empty directory) |
| Info-ZIP UnZip 6.0 (`unzip -t -P secret`, `unzip`) | store, deflate, bzip2, each plain and with ZipCrypto: OK, extraction identical, modes 0755 and mtimes restored; AES, LZMA, zstd and xz are not supported by UnZip 6.0 |
| Python 3.11 `zipfile.testzip()` | store, deflate, bzip2, lzma, plain and ZipCrypto: OK (Python reads neither AES nor zstd/xz) |

An archive of 65600 entries (Zip64 end records) passed `unzip -t`, `7zz t`
and Python `zipfile` (65600 entries, `testzip()` clean).
