# SquashFS fixtures

All images were written by `mksquashfs` 4.5.1 (squashfs-tools, Debian 12) in the
Docker image `cvt-archive-tools:1` (linux/amd64), running as root with `TZ=UTC`,
from the synthetic tree built by `../make_tree.py t` (see `../SOURCES.md`).
Common options, written `$O` below:

```sh
O="-all-root -mkfs-time 1623758400 -all-time 1623758400 -noappend -no-xattrs -quiet -no-progress"
```

`-all-root` stores uid/gid 0, `-mkfs-time`/`-all-time` pin every time to
2021-06-15T12:00:00Z, `-no-xattrs` leaves out extended attributes.

| File | Content | Command |
| --- | --- | --- |
| `tree_gzip.sqfs` | the whole tree, gzip (zlib) | `mksquashfs t tree_gzip.sqfs -comp gzip $O` |
| `tree_xz.sqfs` | the whole tree, xz | `mksquashfs t tree_xz.sqfs -comp xz $O` |
| `tree_lzma.sqfs` | the whole tree, legacy lzma (LZMA_Alone streams) | `mksquashfs t tree_lzma.sqfs -comp lzma $O` |
| `tree_lzo.sqfs` | the whole tree, LZO1X | `mksquashfs t tree_lzo.sqfs -comp lzo $O` |
| `tree_lz4.sqfs` | the whole tree, LZ4 blocks | `mksquashfs t tree_lz4.sqfs -comp lz4 $O` |
| `tree_zstd.sqfs` | the whole tree, Zstandard | `mksquashfs t tree_zstd.sqfs -comp zstd $O` |
| `tree_uncompressed.sqfs` | the whole tree, inodes, data and fragments stored uncompressed | `mksquashfs t tree_uncompressed.sqfs -noI -noD -noF $O` |
| `sparse_gzip.sqfs` | directory `s`: `big.txt` (the lorem paragraph 400 times, 178400 bytes, three 64 KiB blocks) and `sparse.bin` (`begin`, zeros, `end` at 300000: 300003 bytes, sparse blocks), no fragments | see below |

`sparse_gzip.sqfs`:

```sh
mkdir s
python3 -c "
f = open('s/sparse.bin', 'wb'); f.write(b'begin'); f.seek(300000); f.write(b'end'); f.close()
import sys; sys.path.insert(0, '.')
from make_tree import LOREM
open('s/big.txt', 'wb').write((LOREM * 400).encode())
"
touch -d @1623758400 s/sparse.bin s/big.txt s
mksquashfs s sparse_gzip.sqfs -comp gzip -b 65536 -no-fragments $O
```

Every file was checked with `unsquashfs -lls` in the container and scanned for
the login name, home-directory paths, local host names, the container hostname and the
creation date (none present; all times are 2021-06-15T12:00:00Z).

## Interop of our output

Not applicable: SquashFS is read only in Convertible (no writer).
