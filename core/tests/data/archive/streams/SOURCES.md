# Single compressed file fixtures

Made from the synthetic tree (`../make_tree.py t`) with gzip 1.12 in a
Debian 12 container (`cvt-archive-tools:1`, `TZ=UTC`).

| File | Content | Command |
| --- | --- | --- |
| lorem_named.gz | lorem.txt, with the name `lorem.txt` and mtime 2021-06-15T12:00:00Z in the header | `gzip -9 -N -c t/lorem.txt > lorem_named.gz` |
| hello_noname.gz | hello.txt, no name, no time | `gzip -1 -n -c t/hello.txt > hello_noname.gz` |

The other single-stream formats (bz2, xz, lzma, zst, lz4, br, lz, Z, zz, sz)
are tested with the codec fixtures in `../codecs/`.

## Interop of our output

`gzip -dc`, `bzip2 -dc`, `xz -dc`, `zstd -dc`, `lz4 -dc`, `lzip -dc` and
`gzip -dc` (for .Z) restore our single-file outputs byte-exactly; `gzip -lN`
shows the stored original name.
