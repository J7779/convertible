# Unix archive fixtures (cpio, ar, deb, rpm, xar, warc)

All files were written in the Docker image `cvt-archive-tools:1` (Debian 12,
linux/amd64), running as root with `TZ=UTC`, hostname `build` and
`LANG=C.UTF-8 LC_ALL=C.UTF-8`, from the synthetic tree built by
`../make_tree.py` into `t` (see `../SOURCES.md`; every file has mtime
2021-06-15T12:00:00Z = 1623758400). Tools: GNU cpio 2.13, GNU ar (binutils
2.40), bsdtar 3.6.2 (libarchive 3.6.2), dpkg-deb 1.21.23, rpmbuild 4.18.0,
Python 3.11.2. Commands run from the parent of `t` unless they `cd t`.
Every file was scanned for the login name, `/Users`, `/home/`, `.local`, the
container paths and the current date: none occurs. The only machine-like
strings are the fixed host name `build` (RPM build host) and the owner name
`root` that RPM headers always carry.

## cpio

| File | Content | Command |
| --- | --- | --- |
| `tree_newc.cpio` | the whole tree including `.`, newc (070701) | `cd t; find . \| sort \| cpio -o -H newc -R 0:0 --reproducible > ../a/tree_newc.cpio` |
| `tree_crc.cpio` | the same, crc (070702, data checksums) | `cd t; find . \| sort \| cpio -o -H crc -R 0:0 --reproducible > ../a/tree_crc.cpio` |
| `tree_odc.cpio` | the same, odc (070707, POSIX octal) | `cd t; find . \| sort \| cpio -o -H odc -R 0:0 --reproducible > ../a/tree_odc.cpio` |
| `tree_bin.cpio` | hello.txt, lorem.txt, empty.txt, exec.sh, `dir/` (with `nested/deep/file.txt`, `empty_dir`), old binary format, little-endian | `cd t; find hello.txt lorem.txt empty.txt exec.sh dir \| sort \| cpio -o -H bin -R 0:0 --reproducible > ../a/tree_bin.cpio` |

The big-endian binary variant is derived in the test from `tree_bin.cpio` by
swapping the bytes of every 16-bit header word.

## ar

| File | Content | Command |
| --- | --- | --- |
| `tree.a` | hello.txt, lorem.txt, noise.bin, exec.sh and `long_member_name_for_gnu_ar.txt` (a copy of the long-named file, `long name\n`); GNU format with a `//` long-name table, deterministic mode (mtime 0, owner 0, mode 644) | `cd t && cp long/long_file_name_*.txt /tmp/long_member_name_for_gnu_ar.txt && ar rcD ../a/tree.a hello.txt lorem.txt noise.bin exec.sh /tmp/long_member_name_for_gnu_ar.txt` |
| `tree_bsd.a` | hello.txt, lorem.txt, exec.sh; BSD format | `bsdtar -cf a/tree_bsd.a --format arbsd -C t hello.txt lorem.txt exec.sh` |
| `tree_bsd_long.a` | hello.txt and `long_member_name_for_bsd_ar.txt` (`long name\n`), BSD `#1/31` long name | `cp t/long/long_file_name_*.txt t/long_member_name_for_bsd_ar.txt; touch -d @1623758400 t/long_member_name_for_bsd_ar.txt; bsdtar -cf out/tree_bsd_long.a --format arbsd -C t hello.txt long_member_name_for_bsd_ar.txt` |

## deb

A package directory `pkg` with `DEBIAN/control` and
`usr/share/doc/cvt-demo/{hello.txt,lorem.txt}` (copies from the tree):

```sh
mkdir -p pkg/DEBIAN pkg/usr/share/doc/cvt-demo
printf 'Package: cvt-demo\nVersion: 1.0\nArchitecture: all\nMaintainer: Nobody <nobody@invalid>\nDescription: synthetic test package\n' > pkg/DEBIAN/control
cp t/hello.txt t/lorem.txt pkg/usr/share/doc/cvt-demo/
find pkg -exec touch -h -d @1623758400 {} +
SOURCE_DATE_EPOCH=1623758400 dpkg-deb --root-owner-group -Zxz --build pkg a/demo_xz.deb
SOURCE_DATE_EPOCH=1623758400 dpkg-deb --root-owner-group -Zgzip --build pkg a/demo_gz.deb
SOURCE_DATE_EPOCH=1623758400 dpkg-deb --root-owner-group -Zzstd --build pkg a/demo_zst.deb
```

| File | Members |
| --- | --- |
| `demo_xz.deb` | debian-binary, control.tar.xz, data.tar.xz |
| `demo_gz.deb` | debian-binary, control.tar.gz, data.tar.gz |
| `demo_zst.deb` | debian-binary, control.tar.zst, data.tar.zst |

## rpm

`cvt-demo.spec` (in this directory) builds a noarch package with
`/usr/bin/cvt-demo` (0755, `#!/bin/sh\necho hello\n`), the directory
`/usr/share/doc/cvt-demo`, `hello.txt`, `lorem.txt` (the tree's files) and a
symlink `link -> hello.txt` in it, all with mtime 1623758400. No packager,
vendor or distribution; build host `build`; build time from
`SOURCE_DATE_EPOCH`. In a directory holding the spec and the tree's
`lorem.txt` copied to `lorem8.txt`, for each `<ext>:<io>` of
`gz:w9.gzdio xz:w6.xzdio zst:w19.zstdio bz2:w9.bzdio`:

```sh
export SOURCE_DATE_EPOCH=1623758400
rpmbuild -bb --quiet cvt-demo.spec --define "_topdir /tmp/r" --define "_buildhost build" \
  --define "lorem $PWD/lorem8.txt" --define "use_source_date_epoch_as_buildtime 1" \
  --define "_binary_payload <io>" --define "packager %{nil}" --define "vendor %{nil}" \
  --define "distribution %{nil}" --define "_build_id_links none" --define "clamp_mtime_to_source_date_epoch 1"
cp /tmp/r/RPMS/noarch/*.rpm demo_<ext>.rpm
```

| File | Payload |
| --- | --- |
| `demo_gz.rpm` | newc cpio, gzip -9 |
| `demo_xz.rpm` | newc cpio, xz -6 |
| `demo_zst.rpm` | newc cpio, zstd -19 |
| `demo_bz2.rpm` | newc cpio, bzip2 -9 |

Rebuilding gives byte-identical files.

## xar

bsdtar writes the TOC creation time and the files' ctime from the clock;
`fix_xar.py` (in this directory) replaces them by 2021-06-15T12:00:00 and
recomputes the TOC checksum (SHA-1 or MD5) in the heap. Nothing else changes.

| File | Content | Command |
| --- | --- | --- |
| `tree.xar` | hello.txt, lorem.txt, `dir/` (with `nested/deep/file.txt`, `empty_dir`), exec.sh; zlib encoding, SHA-1 checksums | `bsdtar -cf a/tree.xar --format xar --uid 0 --gid 0 --uname "" --gname "" -C t hello.txt lorem.txt dir exec.sh && python3 fix_xar.py a/tree.xar` |
| `tree_xz_md5.xar` | hello.txt, lorem.txt, `link` (symlink to hello.txt); xz encoding, MD5 file and TOC checksums | `bsdtar -cf out/tree_xz_md5.xar --format xar --options xar:compression=xz,xar:checksum=md5,xar:toc-checksum=md5 --uid 0 --gid 0 --uname "" --gname "" -C t hello.txt lorem.txt link && python3 fix_xar.py out/tree_xz_md5.xar` |
| `tree_bz2.xar` | hello.txt, noise.bin; bzip2 encoding | `bsdtar -cf out/tree_bz2.xar --format xar --options xar:compression=bzip2 --uid 0 --gid 0 --uname "" --gname "" -C t hello.txt noise.bin && python3 fix_xar.py out/tree_bz2.xar` |

## warc

No WARC writer is part of the tool image, so `make_warc.py` (in this
directory) writes the records directly from the WARC 1.0 / 1.1 specification
(ISO 28500): version line, `WARC-Type`, `WARC-Date`, `WARC-Record-ID`,
`WARC-Target-URI`, `Content-Type`, `WARC-Block-Digest` (SHA-1, base32),
`Content-Length`, CRLF line ends, two CRLFs after each block.

```sh
cp t/lorem.txt lorem8.txt
python3 make_warc.py lorem8.txt .
```

| File | Content |
| --- | --- |
| `demo.warc` | WARC/1.0 (target URIs in angle brackets, as in the 1.0 examples): warcinfo; request; responses for `http://example.com/`, `https://example.com/docs/page.html?lang=en&x=1` (twice, different bodies), `http://example.com/lorem.txt` (chunked transfer coding, 1000-byte chunks), `http://example.org/hello.txt` (gzip content coding); a resource record for `http://example.org/a/b/`; metadata and revisit records |
| `demo.warc.gz` | the same records as WARC/1.1, each in its own gzip member (level 9, no name, mtime 0) |

## Interop of our output

Written by our writers (the `convertible` command-line tool converting
`../tar/tree_pax.tar` with `--opt strip_metadata=false`) and checked in
`cvt-archive-tools:1` against a fresh `make_tree.py` tree:

| Our output | Independent tool | Result |
| --- | --- | --- |
| cpio (newc) | GNU cpio 2.13 `cpio -idm` | every file byte-identical, same types, modes and symlink (`diff -r --no-dereference` and `find -printf '%p %y %m'`); file mtimes restored |
| cpio (newc) | bsdtar 3.6.2 `bsdtar -xpf` | identical, as above |
| cpio from `demo_gz.rpm` | GNU cpio `cpio -itv` | lists the 9 entries with modes, owner root, 2021 dates, `link -> hello.txt` |
| ar (GNU) | binutils 2.40 `ar x`, `ar tv` | all 8 files (base names) byte-identical; owner 0/0, modes, dates as written |
| ar (GNU) | bsdtar 3.6.2 `bsdtar -xf` | all 8 files byte-identical; like for binutils' own output, libarchive also reports the `//` name table as a member named "/" and exits with a warning |
| shar | dash 0.5.12 (`sh`), bash 5.2, GNU sed 4.9, coreutils 9.1 `base64` | the extracted tree is identical to the reference (content, types, modes, symlink) |
| shar | macOS `/bin/sh` with BSD sed and base64 | identical |
