# tar fixtures

All made from the synthetic tree (`../make_tree.py t`) in a Debian 12
container (`cvt-archive-tools:1`, linux/amd64, `TZ=UTC`, `LANG=C.UTF-8`, run as
root, host name `build`) with GNU tar 1.34, bsdtar 3.6.2 (libarchive) and the
compressors listed in `../codecs/SOURCES.md`. Shell variables used below:

    T="--sort=name --owner=0 --group=0 --numeric-owner --mtime=@1623758400"
    PAX="--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime"

| File | Content | Command |
| --- | --- | --- |
| tree_gnu.tar | whole tree | `tar -cf tree_gnu.tar --format=gnu $T -C t .` |
| tree_pax.tar | whole tree | `tar -cf tree_pax.tar --format=posix $PAX $T -C t .` |
| tree_ustar.tar | hello, lorem, noise, empty, exec.sh, dir/, link | `tar -cf tree_ustar.tar --format=ustar $T -C t hello.txt lorem.txt noise.bin empty.txt exec.sh dir link` |
| tree_v7.tar | hello, lorem, empty, exec.sh, the deep file, link | `tar -cf tree_v7.tar --format=v7 $T -C t hello.txt lorem.txt empty.txt exec.sh dir/nested/deep/file.txt link` |
| tree_bsd_pax.tar | whole tree | `bsdtar -cf tree_bsd_pax.tar --format pax --uid 0 --gid 0 --uname "" --gname "" -C t .` |
| hardlink.tar | `a.txt` (= hello.txt) and its hard link `b.txt` | `cp t/hello.txt s/a.txt; ln -f s/a.txt s/b.txt; tar -cf hardlink.tar --format=gnu $T -C s a.txt b.txt` |
| sparse_gnu.tar.gz | `sparse.bin`: "begin", 199992 zero bytes, "end" (200003 bytes) | `printf begin > s/sparse.bin; truncate -s 200000 s/sparse.bin; printf end >> s/sparse.bin; tar -cf sparse_gnu.tar --format=gnu --sparse $T -C s sparse.bin; gzip -9 -n` |
| sparse_pax10.tar.gz | same file | `tar -cf ... --format=posix --sparse --sparse-version=1.0 $PAX $T -C s sparse.bin; gzip -9 -n` |
| sparse_pax01.tar.gz | same file | `tar -cf ... --format=posix --sparse --sparse-version=0.1 $PAX $T -C s sparse.bin; gzip -9 -n` |
| tree.tar.gz | tree_pax.tar | `gzip -9 -n -c tree_pax.tar` |
| tree.tar.bz2 | tree_pax.tar | `bzip2 -9 -c tree_pax.tar` |
| tree.tar.xz | tree_pax.tar | `xz -6 -c tree_pax.tar` |
| tree.tar.zst | tree_pax.tar | `zstd -q -19 -c tree_pax.tar` |
| tree.tar.lz4 | tree_pax.tar | `lz4 -q -9 -c tree_pax.tar` |
| tree.tar.lz | tree_pax.tar | `lzip -9 -c tree_pax.tar` |
| tree.tar.lzma | tree_pax.tar | `xz --format=lzma -c tree_pax.tar` |
| tree.tar.Z | tree_pax.tar | `compress -c tree_pax.tar` |
| tree.tar.br | tree_pax.tar | `brotli -q 9 -c tree_pax.tar` |

The sparse tars are stored gzip-compressed (`gzip -9 -n`) because they are
mostly zero blocks; the tests decompress them first. Every file was scanned
for the login name, home paths, the host name and the current date.

## Interop of our output

Our tar output (pax, gnu and ustar flavours) and every compressed tar.*
variant written from the tree was extracted byte-exactly (content, modes,
mtimes, symlink) by GNU tar 1.34 and bsdtar 3.6.2 in the container, after
decompressing with the reference tool of each format.
