# Compression codec fixtures

Raw compressed streams of files from the synthetic tree (`../make_tree.py`):
`lorem.txt` (3568 bytes), `noise.bin` (4096 bytes), `hello.txt`, `empty.txt`,
`exec.sh`, and `mixed.bin` = `lorem.txt` followed by `noise.bin` (7664 bytes;
the tests rebuild it in memory). Made in a Debian 12 container
(`cvt-archive-tools:1`, linux/amd64, `TZ=UTC`, run as root) with the
distribution packages: bzip2 1.0.8, xz-utils 5.4.1, lzip 1.23, zstd 1.5.4,
python3-zstandard 0.19, lz4 1.9.4, brotli 1.0.9, python3-brotli 1.0.9,
python3-snappy 0.6, ncompress 4.2.4.6, Python 3.11 zlib. From inside a
directory holding `t/` (the tree) and `mixed.bin`:

| File | Command |
| --- | --- |
| lorem.txt.bz2 | `bzip2 -9 -c t/lorem.txt` |
| noise.bin.bz2 | `bzip2 -1 -c t/noise.bin` |
| empty.bz2 | `bzip2 -c t/empty.txt` |
| two_streams.bz2 | `bzip2 -c t/hello.txt` and `bzip2 -c t/lorem.txt`, concatenated |
| lorem.txt.xz | `xz -6 -c t/lorem.txt` |
| noise_crc32.xz | `xz -0 --check=crc32 -c t/noise.bin` |
| mixed_sha256.xz | `xz -9e --check=sha256 -c mixed.bin` |
| hello_nocheck.xz | `xz --check=none -c t/hello.txt` |
| lorem_blocks.xz | `xz --block-size=1000 -c t/lorem.txt` |
| mixed_x86.xz | `xz --x86 --lzma2 -c mixed.bin` |
| mixed_delta4.xz | `xz --delta=dist=4 --lzma2 -c mixed.bin` |
| mixed_arm64.xz | `xz --arm64 --lzma2 -c mixed.bin` |
| two_streams.xz | `xz -c t/hello.txt` twice, concatenated |
| empty.xz | `xz -c t/empty.txt` |
| lorem.txt.lzma | `xz --format=lzma -c t/lorem.txt` |
| lorem.txt.lz | `lzip -9 -c t/lorem.txt` |
| two_members.lz | `lzip -0 -c t/hello.txt` twice, concatenated |
| lorem_1.zst | `zstd -q -1 -c t/lorem.txt` |
| mixed_19.zst | `zstd -q -19 -c mixed.bin` |
| noise_nocheck.zst | `zstd -q --no-check -c t/noise.bin` |
| lorem_stream.zst | `cat t/lorem.txt \| zstd -q -3 -c` (no content size) |
| frames_skippable.zst | python-zstandard: level-3 frame of `first frame ` (checksum), a skippable frame (magic 0x184D2A50, 5 bytes `12345`), a level-9 frame of lorem.txt without content size |
| runs.zst | python-zstandard level 5 of 70000 × `a` + 70000 × `b` |
| empty.zst | `zstd -q -c t/empty.txt` |
| lorem_1.lz4 | `lz4 -q -1 -c t/lorem.txt` |
| mixed_9_linked.lz4 | `lz4 -q -9 -BD --content-size -c mixed.bin` |
| noise_nocrc.lz4 | `lz4 -q -1 -B4 --no-frame-crc -c t/noise.bin` |
| lorem_legacy.lz4 | `lz4 -q -l -c t/lorem.txt` |
| empty.lz4 | `lz4 -q -c t/empty.txt` |
| lorem_q0.br, lorem_q5.br | `brotli -q 0` / `-q 5 -c t/lorem.txt` |
| mixed_q11.br | `brotli -q 11 -w 16 -c mixed.bin` |
| exec_q9.br | `brotli -q 9 -c t/exec.sh` |
| empty.br | `brotli -c t/empty.txt` |
| prose.txt | the sentence "The quick brown fox jumps over the lazy dog. Their house is near the river, and they would rather go there than anywhere else in the world. " three times (420 bytes) |
| prose_q11.br, prose_q4.br | python-brotli `brotli.compress(prose, quality=11, mode=MODE_TEXT)` / `quality=4` |
| lorem.snappy | python-snappy `snappy.compress(lorem)` (raw format) |
| mixed.sz | Snappy framing stream of 20 × lorem.txt + noise.bin: stream identifier, 64 KiB chunks whose bodies are `snappy.compress(chunk)` (or the chunk itself when that is not smaller) with the masked CRC-32C, and a 3-byte padding chunk. python-snappy's own framing writer is broken in this release, so the frame headers were assembled by a 30-line Python script following the framing specification; the compressed chunk bodies come from the snappy library |
| lorem.txt.Z | `compress -c t/lorem.txt` (16 bits) |
| mixed_b12.Z | `compress -b 12 -c mixed.bin` |
| lorem.zz | Python `zlib.compress(lorem, 9)` |

`compress -b 9` output was not kept: ncompress 4.2.4.6 and gzip both fail to
decode the files it writes at 9 bits.

## Interop of our output

Our encoders' output for three corpora (1.1 MB text, a 1.2 MB tar of mixed
files, a 5.3 MB executable) at levels 1, 3, 6 and 9 was decoded byte-exactly
by `bzip2 -d`, `xz -d`, `zstd -d`, `lz4 -d`, `gzip -d` (also for our .Z
output) in the container. Our `.lzma`, lzip and 7z/xz LZMA2 streams were also
checked with `xz --format=lzma -d`, `lzip -d` and `7zz t` during development.
Brotli interop is recorded in `../brotli/SOURCES.md`, PPMd in `../ppmd/SOURCES.md`.
