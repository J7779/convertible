# Apple disk image (DMG) fixtures

Made on macOS 27 with `hdiutil` (the system disk-image tool) and `ditto`, with
`TZ=UTC COPYFILE_DISABLE=1`, from the synthetic tree built by
`python3 -I ../make_tree.py t` (see `../SOURCES.md`). Volume name `CVT`,
non-journaled HFS+ (`-fs HFS+`) or HFSX, no Spotlight index.

hdiutil copies the creator's uid/gid and stamps several dates with the current
time (volume header, root and private folders, attribute-modification dates,
the Finder "date added"), and copies the `com.apple.provenance` attribute.
Every volume was therefore first written as a raw read/write image (UDRW),
cleaned with `hfs_scrub.py` (in this directory: owners -> 0, recent dates ->
2021-06-15T12:00:00Z, provenance values zeroed; contents, names, modes and the
B-tree structure untouched) and only then converted to the final formats.

```sh
make() { # name layout fs srcdir
  hdiutil create -quiet -srcfolder "$4" -fs "$3" -volname CVT -format UDRW -layout "$2" -nospotlight "raw_$1.dmg"
  python3 -I hfs_scrub.py "raw_$1.dmg"
}
make tree GPTSPUD HFS+ t     # GUID partition table
make apm SPUD HFS+ t         # Apple partition map
for f in UDZO UDBZ ULFO ULMO UDRO UDCO; do hdiutil convert -quiet raw_tree.dmg -format $f -o tree_$f.dmg; done
hdiutil convert -quiet raw_apm.dmg -format ULMO -o tree_apm_ULMO.dmg
gzip -9 -n tree_UDRO.dmg tree_UDCO.dmg
```

The HFSX image without a partition map holds a hard link pair and a file with
HFS (decmpfs) compression, which `ditto --hfsCompression` writes as LZVN
(decmpfs type 7, inline). It is written to the mounted volume (mounting a raw
image writes nothing else that remains; the scrub runs afterwards):

```sh
mkdir x mnt
printf 'shared content\n' > x/a.txt
ln x/a.txt x/b.txt
python3 -I -c "import sys; sys.path.insert(0, '..'); from make_tree import LOREM; open('lorem_src.txt', 'w').write(LOREM * 40)"
cp lorem_src.txt x/plain.txt
touch -h -t 202106151200.00 x/a.txt x/plain.txt x
hdiutil create -quiet -srcfolder x -fs HFSX -volname CVT -format UDRW -layout NONE -nospotlight raw_bare.dmg
hdiutil attach -quiet -nobrowse -owners on -mountpoint mnt raw_bare.dmg
ditto --hfsCompression lorem_src.txt mnt/packed.txt
touch -t 202106151200.00 mnt/packed.txt
hdiutil detach -quiet mnt
python3 -I hfs_scrub.py raw_bare.dmg
hdiutil convert -quiet raw_bare.dmg -format UDZO -o links_hfsx_UDZO.dmg
gzip -9 -n -c raw_bare.dmg > links_hfsx_raw.dmg.gz
```

| File | Chunks / layout | Content |
| --- | --- | --- |
| `tree_UDZO.dmg` | zlib (0x80000005), GPT | the whole tree |
| `tree_UDBZ.dmg` | bzip2 (0x80000006), GPT | the whole tree |
| `tree_ULFO.dmg` | LZFSE (0x80000007), GPT | the whole tree |
| `tree_ULMO.dmg` | LZMA (0x80000008, xz streams), GPT | the whole tree |
| `tree_UDRO.dmg.gz` | raw (1) and zero-fill (2) chunks, GPT; gzip-compressed for size | the whole tree |
| `tree_UDCO.dmg.gz` | ADC (0x80000004), GPT; gzip-compressed for size | the whole tree |
| `tree_apm_ULMO.dmg` | LZMA (xz), Apple partition map | the whole tree |
| `links_hfsx_UDZO.dmg` | zlib, bare HFSX volume (no partition map) | `a.txt` and `b.txt` (one hard-linked file, `shared content\n`), `plain.txt` (lorem x 40), `packed.txt` (the same text, LZVN HFS compression) |
| `links_hfsx_raw.dmg.gz` | the same volume as a raw image without a UDIF trailer (UDRW), gzip-compressed | as above |

HFS+ stores the unicode name decomposed (NFD); the reader returns it
precomposed, as in the tree. `hdiutil verify` accepts the images and
`hdiutil attach -readonly` shows the tree with identical contents.
Every file and the raw volumes were scanned for the login name, home-directory
paths, local host names, the hostname and recent dates, also as binary HFS and Unix
timestamps (none present; the only matches were inside the noise file and
compressed data).

## Interop of our output

Not applicable: DMG is read only in Convertible (no writer).
