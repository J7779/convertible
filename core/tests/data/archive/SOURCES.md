# Archive family fixtures

The archive and compression fixtures are grouped in sub-directories, one per
format or codec group; each sub-directory has its own `SOURCES.md` listing
every file, the tool and version that wrote it and the exact command:

`codecs/` (bzip2, xz, lzma, lzip, zstd, lz4, brotli, snappy, compress, zlib),
`brotli/`, `ppmd/`, `apple_codecs/` (LZFSE, ADC), `legacy_codecs/` (Deflate64,
MSZIP, LZX, Quantum, XPRESS, LHA, ARJ, LZO, PKZIP legacy), `tar/`, `streams/`,
`zip/`, `7z/`, `rar/`, `unix/` (cpio, ar, deb, rpm, WARC, xar), `iso/`, `cab/`
(CAB, CHM), `wim/`, `squashfs/`, `dmg/`, `legacy/` (LHA, ARJ, ARC).

## The synthetic tree

Almost every archive fixture packs the same small file tree, built by
`make_tree.py` (in this directory) into an empty directory:

```sh
python3 make_tree.py t      # then run the archiver inside or on "t"
```

| Path | Content | Mode |
| --- | --- | --- |
| `hello.txt` | `Hello, archive!\n` (16 bytes) | 0644 |
| `lorem.txt` | the classic lorem ipsum paragraph, 8 times (3568 bytes) | 0644 |
| `noise.bin` | 4096 bytes of xorshift32 noise (seed 2463534242) | 0644 |
| `empty.txt` | empty | 0644 |
| `exec.sh` | `#!/bin/sh\necho hello\n` | 0755 |
| `dir/nested/deep/file.txt` | `deep file\n` | 0644 |
| `dir/empty_dir/` | empty directory | 0755 |
| `unicode/ünïcödé-名前.txt` | `unicode name\n` (NFC name) | 0644 |
| `long/long_file_name_xxx…x.txt` | `long name\n`; the name is 124 bytes | 0644 |
| `link` | symbolic link to `hello.txt` | 0777 |

Every file, directory and the link carry the modification time
2021-06-15T12:00:00Z (1623758400). `core/tests/families/archive/tree_fixture.hpp`
generates the same content in memory and compares decoded archives with it
entry by entry (path, type, exact bytes, permission bits, mtime, link target).

Archivers are always run so that no user or machine data is stored: numeric
owner 0/0 and no owner names (`--owner=0 --group=0 --numeric-owner`), no
extended attributes or resource forks, UTC time zone (`TZ=UTC`), no comments,
and `zip -X` (no uid/gid extra fields). Every fixture has been scanned with
`LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local"` and contains none of them.
Tools ran either on macOS (zip, tar, gzip, bzip2, xz, compress) or in a
throwaway Debian 12 container (`debian:bookworm`, linux/amd64) with the
distribution packages named in each `SOURCES.md`; RAR 4 archives come from
RAR 6.23 in an `ubuntu:22.04` container (multiverse package `rar`).
