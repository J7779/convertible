# Apple codec fixtures (LZFSE / LZVN)

Written on macOS 27 with Apple's `compression_tool` (the system Compression
library), `compression_tool -encode -a lzfse -i <input> -o <output>`. The
library picks the block type itself: stored ("bvx-") for tiny or
incompressible input, LZVN ("bvxn") below 4 KiB, LZFSE v2 ("bvx2") above.
The tests generate the expected content themselves; inputs come from `gen.py`
of `../ppmd/SOURCES.md` and `../make_tree.py`.

| File | Input | Blocks |
| --- | --- | --- |
| `hello.lzfse` | `Hello, archive!\n` (16 bytes) | bvx- |
| `empty.lzfse` | empty file | bvx- (0 bytes) |
| `noise2000.lzfse` | `noise(2000)` | bvx- |
| `lorem.lzfse` | `lorem.txt` of the tree (3568 bytes) | bvxn |
| `corpus4000.lzfse` | `text_corpus(4000)` | bvxn |
| `noise.lzfse` | `noise(4096)` (`noise.bin` of the tree) | bvx2, literals only |
| `mixed.lzfse` | `lorem.txt` + `noise(4096)` | bvx2 |
| `corpus60k.lzfse` | `text_corpus(60000)` | bvx2 |
| `corpus130k.lzfse` | `text_corpus(130000)` | two bvx2 blocks, matches across the boundary |

`compression_tool` cannot write v1 ("bvx1") headers; the tests rewrite the
v2 header of `corpus60k.lzfse` into the equivalent v1 header (payload
unchanged) to cover that path. ADC has no fixtures here: its tests use
hand-assembled streams and a small encoder written in the test, and the DMG
tests decode ADC from real disk images.

## Interop of our output

Decode only: there is no LZFSE, LZVN or ADC encoder.
