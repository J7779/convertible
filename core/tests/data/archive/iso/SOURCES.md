# ISO 9660 / UDF fixtures

Disc images are compressed with `gzip -9 -n` for the repository; the tests
decompress them with `compression::decompress(Method::Gzip, ...)`. Every
image was made in the archive tools container (Debian 12, linux/amd64, run as
root with `TZ=UTC`) from the synthetic tree of `../make_tree.py` (`t/`, with
the tree root's own time also set: `touch -d @1623758400 t`).

| File | Content | Tool | Command |
| --- | --- | --- | --- |
| `tree_rr_joliet.iso.gz` | the whole tree; Rock Ridge + Joliet (long) names, level 3 | xorriso 1.5.4 | `SOURCE_DATE_EPOCH=1623758400 xorriso -as mkisofs -quiet -R -J -joliet-long -iso-level 3 -V CVT --modification-date=2021061512000000 -o rr.iso t` |
| `plain.iso.gz` | `hello.txt`, `lorem.txt`; level 1 names, no extensions | xorriso 1.5.4 | `SOURCE_DATE_EPOCH=1623758400 xorriso -as mkisofs -quiet -iso-level 1 -V CVT --modification-date=2021061512000000 -o plain.iso t/hello.txt t/lorem.txt` |
| `zisofs.iso.gz` | `hello.txt`, `lorem.txt` with Rock Ridge ZF (zisofs) compression | xorriso 1.5.4 | see below |
| `udf_hybrid.iso.gz` | the tree without the symbolic link and the long name: UDF 1.02 + ISO 9660 level 1 names | genisoimage 1.1.11 | see below |

zisofs (xorriso's native commands; `z/` holds copies of `hello.txt` and `lorem.txt`):

```sh
mkdir z && cp -p t/hello.txt t/lorem.txt z/ && touch -d @1623758400 z
SOURCE_DATE_EPOCH=1623758400 xorriso -report_about SORRY -outdev z.iso -volid CVT -map z / \
  -set_filter_r --zisofs / -- -commit
```

UDF (`u/` holds copies of `hello.txt lorem.txt noise.bin empty.txt exec.sh dir unicode`,
`cp -a`, and `touch -d @1623758400 u`):

```sh
genisoimage -quiet -input-charset utf-8 -udf -V CVT -o udf.iso u
```

genisoimage ignores `SOURCE_DATE_EPOCH` and records the time of the run in the
volume descriptors and the UDF descriptors. Those build-time dates were then
replaced with 2021-06-15T12:00:00Z by a small Python script: every 17-digit
decimal date of that day became `2021061512000000`, every 7-byte directory
record date of that day became `79 06 0F 0C 00 00 00`, every 12-byte UDF
timestamp of that day became 2021-06-15 12:00:00 (type 1, offset 0), and each
UDF descriptor tag was resealed (descriptor CRC-ITU-T over the tag's CRC
length, then the tag checksum). `7zz x -tudf` extracts the patched image and
the result is identical to `u/` (`diff -r`).

No fixture contains a user name, host name, absolute path or the date it was
made (scanned with `grep -aiE` for the login name, `/Users`, `/home/`, `.local`,
`/w/`, `/tmp/`, `build` and the year of the run, in UTF-8 and UTF-16).

## Interop of our output

Our writer's image of the tree (`convertible convert tree_pax.tar -t iso`) was
checked in the same container:

- `xorriso -osirrox on -indev ours.iso -extract / out`: identical to the tree
  (`diff -r`), permissions, times and the symbolic link included;
  `xorriso -check_media`: all blocks good.
- `bsdtar -xf ours.iso` (libarchive 3.6.2): identical to the tree.
- `isoinfo -d / -R -l / -J -f` (genisoimage 1.1.11): "Joliet with UCS level 3
  found", "Rock Ridge signatures version 1 found", the Rock Ridge listing shows
  names, modes, owners 0, 2021-06-15 times and `link -> hello.txt`; the Joliet
  listing shows every name.
- `7zz x` (7-Zip 26, which reads the Joliet tree and ignores Rock Ridge):
  identical except that the link is absent and the 124-byte name is cut to 64
  characters, as Joliet allows.

A tree with clashing ISO names (`a-b.txt`, `a_b.txt`, `A B.TXT`), a 240-byte
name, 13 directory levels, a 1 MiB file and a 286-byte link target (Rock Ridge
continuation areas, split NM and SL entries) also extracts identically with
xorriso and bsdtar, and isoinfo lists the full names and link target.
