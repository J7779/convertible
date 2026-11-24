# Legacy codec fixtures

Raw compressed streams for the decode-only legacy codecs (Deflate64, LZX,
XPRESS, LHA, ARJ, LZO1X), written by independent tools in a throwaway
Debian 12 container (`cvt-archive-tools:1`, linux/amd64, run as root with
`TZ=UTC`). Each stream was cut out of the container file the tool wrote, so
that no archive header (names, dates, owners) is stored; the tests in
`core/tests/families/archive/codec_legacy_test.cpp` decode them and compare
with content they regenerate.

## Inputs

| Name | Content |
| --- | --- |
| `mixed.bin` | `lorem.txt` followed by `noise.bin` of the synthetic tree (`../make_tree.py`), 7664 bytes (the tests' `codec::mixed()`) |
| `text40k.txt` | 40 000 bytes of `codec::text_corpus` (LCG `state = state * 1103515245 + 12345` from 12345; word `(state >> 16) % 20` of the 20-word list in `codec_test_util.hpp`, then `".\n"` when `(state >> 8) % 11 == 0`, else a space) |
| `x86.bin` | 45 000 bytes of x86-like code: records `E8 rel32 83 C4 xx 55 8B EC` with `rel32` pointing at `(state >> 8) % 30000` (same LCG from 1; `xx = (state >> 4) & 0x0C`) — `legacy::x86_like()` in `codec_legacy_util.hpp` |
| `a.html` | `<html><body><pre>\n` + `text_corpus(100000)` + `\n</pre></body></html>\n` (100 040 bytes) |

## Files

| File | Tool | Command (then stream extraction) |
| --- | --- | --- |
| `mixed.lh1`, `mixed.lh4` .. `mixed.lh7`, `mixed.lzs`, `mixed.lz5`, `text40k.lh1`, `text40k.lh5`, `text40k.lh7` | jLHA (libjlha-java 0.0.20050504, OpenJDK 17) | `java -cp /usr/share/java/jlha.jar LhRaw.java -lhN- <input> <output> <check.lzh>`: `LhRaw` writes `CompressMethod.connectEncoder(out, method, LhaProperty.getProperties())` over the whole input (the raw stream) and, for checking, the same data as a level-2 `.lzh` with `LhaOutputStream`; every `.lzh` passed `lhasa t` (lhasa 0.3.1) |
| `mixed.arj1` .. `mixed.arj4`, `text40k.arj1`, `text40k.arj4` | ARJ32 3.10 (Debian arj 3.10.22) | `arj a -mN -i -y x.arj <input>`; the compressed data of the single file entry (after the main header, the file header with its CRC and the empty extended header) |
| `mixed_1.lzo1x`, `mixed_9.lzo1x`, `text40k_9.lzo1x` | lzop 1.04 (LZO 2.10) | `lzop -1` / `lzop -9 -f -o x.lzo <input>`; the data of the only block (LZO1X-1 / LZO1X-999), which is compressed in each case |
| `twice.deflate64` | 7-Zip 26.02 | `7zz a -tzip -mm=Deflate64 -mx=9 x.zip twice.txt` (`twice.txt` = `text40k.txt` twice, 80 000 bytes: distances above 32 KiB); the entry data |
| `text40k_maximum.chunks`, `x86_maximum.chunks` (LZX), `text40k_fast.chunks`, `x86_fast.chunks` (XPRESS) | wimlib-imagex 1.13.5 | `wimcapture src x.wim --compress=maximum` / `--compress=fast` with `src` holding `text40k.txt` and `x86.bin`; each file's resource split at its chunk table into 32 KiB chunks, stored as a list of (LE32 uncompressed size, LE32 compressed size, chunk bytes); a chunk whose sizes are equal is stored uncompressed |
| `chm_content.lzx` | chmcmd (Free Pascal fp-utils 3.2.2) | `chmcmd test.hhp` for a project with `a.html` only (options: `Compatibility=1.1 or later`, `Compiled file=test.chm`, `Default topic=a.html`, `Language=0x409`, `Title=Test`); the `::DataSpace/Storage/MSCompressed/Content` stream. Its parameters: LZXC version 2, window 2 x 32 KiB (`window_bits` 16), reset interval 2 x 32 KiB, 106 888 bytes uncompressed in 19 654 compressed; reset table offsets 0, 7404, 12774, 18742 (end 19654); `/a.html` at offset 2751, length 100 040 (also in the section: `$OBJINST`, `#IDXHDR`, `#STRINGS`) |

The MSZIP and the first Deflate64 tests use `../cab/mszip.cab` and
`../zip/deflate64.zip` (see their directories' `SOURCES.md`).

No tool exists for PKZIP shrink/reduce/implode, CAB Quantum, plain MS-XCA
LZ77 or CAB LZX in the container (7-Zip, Info-ZIP zip, gcab and arc cannot
write them); those methods are tested with streams assembled by the test
itself (`codec_legacy_synth_test.cpp`).

All files were scanned with `LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local"`
and for the container hostname and paths; none matched.

## Interop of our output

Not applicable: these codecs are decode-only.
