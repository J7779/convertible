# Windows Imaging (WIM) fixtures

Made in the archive tools container (Debian 12, linux/amd64, root, `TZ=UTC`)
with wimlib-imagex 1.13.5 from the synthetic tree of `../make_tree.py` (`t/`,
with `touch -d @1623758400 t` for the root directory as well). wimlib stores
the symbolic link as a reparse point.

| File | Compression | Command |
| --- | --- | --- |
| `tree_none.wim` | none, with wimlib's UNIX data (permissions, owner 0) | `wimcapture t tree_none.wim CVT --compress=none --no-acls --unix-data` |
| `tree_fast.wim` | XPRESS (Huffman), 32 KiB chunks | `wimcapture t tree_fast.wim CVT --compress=fast --no-acls` |
| `tree_maximum.wim` | LZX, 32 KiB chunks | `wimcapture t tree_maximum.wim CVT --compress=maximum --no-acls` |
| `tree_lzms.wim` | LZMS, 128 KiB chunks, not solid | `wimcapture t tree_lzms.wim CVT --compress=lzms --no-acls` |
| `tree_solid.wim` | LZMS solid resource (ESD-style, version 0xE00) | `wimcapture t tree_solid.wim CVT --solid --no-acls` |

wimcapture writes the time of the run into the image's XML description
(`CREATIONTIME`, `LASTMODIFICATIONTIME`). Both were replaced afterwards, in
place and with the same length, by the FILETIME of 2021-06-15T12:00:00Z
(`HIGHPART 0x01D761DD`, `LOWPART 0xF7932000`); the XML resource is not covered
by a hash (no integrity table). `wimlib-imagex verify` accepts every file and
`wimlib-imagex apply <file> 1 out` gives a tree identical to `t/` (`diff -r`).
The image GUID in the header is random by design and identifies nothing.

No fixture contains a user name, host name, absolute path or the date it was
made (scanned with `grep -aiE` for the login name, `/Users`, `/home/`,
`.local`, `/w/`, `/tmp/`, `build` and the year of the run, in UTF-8 and UTF-16).

## Interop of our output

WIM is read only.
