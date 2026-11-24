# 7z fixtures

All files were written in the Docker image `cvt-archive-tools:1` (Debian 12,
linux/amd64), running as root with `TZ=UTC` and `LANG=C.UTF-8 LC_ALL=C.UTF-8`,
from the synthetic tree built by `../make_tree.py` (see `../SOURCES.md`; every
file has mtime 2021-06-15T12:00:00Z). Commands run from inside the tree
(`cd t`) and write to `../a/`. `-mtc=off -mta=off` keeps 7-Zip from storing
creation and access times, so only the fixed modification time is recorded.

Tools: 7-Zip 26.02 (`7zz`), the Debian `p7zip-full` package (`7z`, p7zip 16.02),
bsdtar 3.6.2 (libarchive 3.6.2).

`mixed.bin` is `lorem.txt` followed by `noise.bin` (7664 bytes), made in the
parent of `t`: `cat t/lorem.txt t/noise.bin > mixed.bin && touch -d @1623758400 mixed.bin`.

| File | Content | Command |
| --- | --- | --- |
| `tree_lzma2.7z` | the whole tree, solid LZMA2 (7-Zip's default), `link` stored as a symlink (`-snl`), LZMA-compressed header | `7zz a -bd -t7z -mtc=off -mta=off -snl ../a/tree_lzma2.7z .` |
| `tree_lzma.7z` | hello.txt, lorem.txt, noise.bin, `dir/` (with `empty_dir`), LZMA, one folder per file | `7zz a -bd -t7z -ms=off -m0=LZMA -mtc=off -mta=off ../a/tree_lzma.7z hello.txt lorem.txt noise.bin dir` |
| `ppmd.7z` | lorem.txt, hello.txt, PPMd (var.H) | `7zz a -bd -t7z -m0=PPMd -mtc=off -mta=off ../a/ppmd.7z lorem.txt hello.txt` |
| `bzip2.7z` | lorem.txt, hello.txt, BZip2 | `7zz a -bd -t7z -m0=BZip2 -mtc=off -mta=off ../a/bzip2.7z lorem.txt hello.txt` |
| `deflate.7z` | lorem.txt, hello.txt, Deflate | `7zz a -bd -t7z -m0=Deflate -mtc=off -mta=off ../a/deflate.7z lorem.txt hello.txt` |
| `deflate64.7z` | lorem.txt, hello.txt, Deflate64 | `7zz a -bd -t7z -m0=Deflate64 -mtc=off -mta=off ../a/deflate64.7z lorem.txt hello.txt` |
| `copy.7z` | lorem.txt, hello.txt, empty.txt, Copy, plain (unencoded) header | `7zz a -bd -t7z -m0=Copy -mhc=off -mtc=off -mta=off ../a/copy.7z lorem.txt hello.txt empty.txt` |
| `bcj2.7z` | mixed.bin through BCJ2 (four packed streams: LZMA2 main, LZMA call/jump, range coder) | `7zz a -bd -t7z -mf=BCJ2 -mtc=off -mta=off ../a/bcj2.7z ../mixed.bin` |
| `arm64.7z` | mixed.bin through the ARM64 filter + LZMA2 | `7zz a -bd -t7z -mf=ARM64 -mtc=off -mta=off ../a/arm64.7z ../mixed.bin` |
| `delta.7z` | mixed.bin through the Delta filter (distance 4) + LZMA2 | `7zz a -bd -t7z -mf=Delta:4 -mtc=off -mta=off ../a/delta.7z ../mixed.bin` |
| `encrypted.7z` | hello.txt, lorem.txt, 7zAES (AES-256) with encrypted header, password `secret` | `7zz a -bd -t7z -mhe=on -psecret -mtc=off -mta=off ../a/encrypted.7z hello.txt lorem.txt` |
| `encrypted_data.7z` | hello.txt, 7zAES data, plain names, password `secret` | `7zz a -bd -t7z -psecret -mtc=off -mta=off ../a/encrypted_data.7z hello.txt` |
| `p7zip.7z` | hello.txt, lorem.txt, exec.sh, `dir/`, written by p7zip | `7z a -bd -t7z -mtc=off -mta=off ../a/p7zip.7z hello.txt lorem.txt exec.sh dir` |
| `bsdtar.7z` | hello.txt, lorem.txt, exec.sh, `dir/`, `link` (symlink), written by libarchive | `bsdtar -cf ../a/bsdtar.7z --format 7zip --uid 0 --gid 0 --uname "" --gname "" hello.txt lorem.txt exec.sh dir link` |

The full script is equivalent to:

```sh
docker run --rm --platform linux/amd64 --memory 2g --hostname build -e TZ=UTC -v "$PWD":/w -w /w cvt-archive-tools:1 sh fx.sh
# fx.sh: python3 make_tree.py t; make mixed.bin (above); cd t; run the commands of the table
```

Every file was scanned with `LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local|/w/|build"`
(nothing found); the only time stored is the tree's fixed modification time.

## Interop of our output

The writer's output for `tree_fixture.hpp`'s tree (directories, the empty
directory, an empty file, exec.sh 0755, the unicode and 124-byte names, the
`link` symlink) was checked in the same container:

| Our archive | `7zz t` | `7zz x -snl` vs the tree (`diff -r`) | `7z t` (p7zip) | `bsdtar -xf` vs the tree |
| --- | --- | --- | --- | --- |
| LZMA2, LZMA, BZip2, Deflate, Copy; each solid and not solid | ok | byte-exact, symlink kept | ok | byte-exact, symlink kept |
| AES-256 + LZMA2, encrypted header, password `secret` (`-psecret`) | ok | byte-exact | ok | (libarchive has no 7zAES) |
| AES-256 + LZMA2, not solid, plain header | ok | byte-exact | ok | (libarchive has no 7zAES) |
| AES-256 + Copy, encrypted header | ok | byte-exact | ok | (libarchive has no 7zAES) |
| empty archive (32-byte signature header only) | ok | - | - | lists nothing |
| one directory only (no streams) | ok | - | - | lists `only/dir/` |
| 1.1 MB of text + 200 kB of noise | ok | - | - | lists both |

`7zz l -slt` and `bsdtar -tvf` show the stored Unix modes (`drwxr-xr-x`,
`-rwxr-xr-x` for exec.sh, `lrwxrwxrwx link -> hello.txt`), owner 0/0 and the
2021-06-15 12:00:00 modification times.
