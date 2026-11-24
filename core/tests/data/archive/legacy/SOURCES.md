# Legacy archive fixtures (LHA/LZH, ARJ, ARC)

All files were written in the Docker image `cvt-archive-tools:1` (Debian 12,
linux/amd64), running as root with `TZ=UTC`, from the synthetic tree built by
`../make_tree.py t` (see `../SOURCES.md`). Commands run from inside the tree
(`cd t`) unless noted.

## LHA / LZH (jlha-utils 0.1.6, jLHA library 0.0.20050504)

`jlha` writes header levels 0, 1 and 2, the DOS time (levels 0/1) or the Unix
time (level 2), and no Unix permissions; it follows no symlinks here because
`link` is not passed, and stores no unicode names here (`unicode/` is not
passed). Level 0 cannot hold the 124-byte name, so `long/` is left out there.
`L` = `hello.txt lorem.txt noise.bin empty.txt exec.sh dir long`.

| File | Content | Command |
| --- | --- | --- |
| `tree_l0_lh0.lzh` | level 0, stored (`-lh0-`), the tree without `long/` | `jlha a0zq ../tree_l0_lh0.lzh hello.txt lorem.txt noise.bin empty.txt exec.sh dir` |
| `tree_l1_lh0.lzh` | level 1, stored | `jlha a1zq ../tree_l1_lh0.lzh $L` |
| `tree_l2_lh0.lzh` | level 2, stored | `jlha a2zq ../tree_l2_lh0.lzh $L` |
| `tree_l2_lh5.lzh` | level 2, `-lh5-` (stored where that is smaller) | `jlha a2o5q ../tree_l2_lh5.lzh $L` |
| `tree_l1_lh6.lzh` | level 1, `-lh6-` | `jlha a1o6q ../tree_l1_lh6.lzh $L` |
| `tree_l2_lh7.lzh` | level 2, `-lh7-` | `jlha a2o7q ../tree_l2_lh7.lzh $L` |

Every archive passes `lhasa t` (Lhasa 0.3.1).

## ARJ (ARJ32 3.10.22, Debian package arj)

The whole tree (`'*'`), recursively. ARJ stores no directory entries here and
follows the symlink (`link` is a copy of `hello.txt`); host OS UNIX, so times
are Unix times and modes are stored. ARJ also records the archive creation
time and each file's ctime ("DTC") from the clock; `arj_fix_times.py` (in this
directory) replaces exactly those fields with 2021-06-15T12:00:00Z and
recomputes the header CRC-32s. Nothing else is changed.

| File | Content | Command |
| --- | --- | --- |
| `tree_m0.arj` | the tree, method 0 (stored) | `arj a -m0 -r -y ../tree_m0.arj '*'; python3 arj_fix_times.py ../tree_m0.arj` |
| `tree_m1.arj` | the tree, method 1 | `arj a -m1 -r -y ../tree_m1.arj '*'; python3 arj_fix_times.py ../tree_m1.arj` |
| `tree_m4.arj` | the tree, method 4 | `arj a -m4 -r -y ../tree_m4.arj '*'; python3 arj_fix_times.py ../tree_m4.arj` |

Every archive passes `arj t` after the time fix.

## ARC (arc 5.21q, Debian package arc)

ARC has no directories, so a flat directory `flat` is used: `hello.txt`,
`lorem.txt`, `noise.bin`, `empty.txt` from the tree plus two files that make
`arc` choose its other methods, all with mtime 2021-06-15T12:00:00Z:

```python
import random, os
open('runs.bin', 'wb').write(b''.join(bytes([i % 7]) * (20 + i % 13) for i in range(200)))
r = random.Random(7)
open('skew.txt', 'wb').write(bytes(r.choice(b'aaaaaaaaaaaaaaaabbbbbbbbccccddef') for _ in range(3000)))
for n in os.listdir('.'):
    os.utime(n, (1623758400, 1623758400))
```

| File | Content | Command (in `flat`) |
| --- | --- | --- |
| `flat.arc` | empty.txt, hello.txt, noise.bin stored; lorem.txt and runs.bin crunched (method 8); skew.txt squeezed (method 4) | `arc aw ../flat.arc hello.txt lorem.txt noise.bin empty.txt runs.bin skew.txt` |
| `flat_squashed.arc` | lorem.txt, skew.txt squashed (method 9) | `arc aqw ../flat_squ.arc lorem.txt skew.txt` (arc truncates the name to 8.3; renamed) |

`nomarch -t` (nomarch 1.4) reports every entry of both archives ok, and
`nomarch` extracts files identical to the originals. `arc t` itself reports a
CRC error for the squeezed `skew.txt`, whose Huffman data ends without the
end-of-data code; nomarch and Convertible stop at the end of the data and get
the original bytes (the CRC matches).

Every file was scanned for the login name, home-directory paths, local host
names, the container hostname and the creation date (none present).

## Interop of our output

Not applicable: these formats are read only in Convertible (no writers).
