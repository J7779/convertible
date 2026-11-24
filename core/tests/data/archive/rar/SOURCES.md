# RAR fixtures

Every archive was written by RARLAB's own `rar` (trial version, as permitted)
in a short-lived Docker container, running as root with
`TZ=UTC LANG=C.UTF-8 LC_ALL=C.UTF-8`, from synthetic content only:

* the tree built by `../make_tree.py` (see `../SOURCES.md`; every file and
  directory has mtime 2021-06-15T12:00:00Z), packed from inside it (`cd t`);
* the inputs written by `make_inputs.py` in this directory
  (`python3 make_inputs.py in`, then packed from inside `in`): `x86.bin`
  (24576 bytes of x86-like code with relative CALL/JMP to a few targets),
  `rgb.bmp` (a 64x64 24-bit gradient), `pcm.wav` (6000 stereo 16-bit
  samples of two sine tones), `table.dat` (3000 8-byte records of slowly
  growing integers), `text.txt` (20000 bytes of lorem ipsum words in
  pseudo-random order), `far.bin` (an 8 KiB pseudo-random block repeated
  across 300 KB, 1 MB and 2.5 MB of zeros; 3832768 bytes), `wrap.bin` (a
  6000-byte pseudo-random block every 40016 bytes; 1040416 bytes),
  `sparse.exe` (640 KiB of zeros with one CALL and one JMP per KiB to four
  absolute targets); all with mtime 2021-06-15T12:00:00Z;
* `comment.txt`: the line `Synthetic RAR test archive comment.`

RAR 5 archives (`rar5_*`): rar 7.01 in `cvt-archive-tools:1` (Debian 12).
RAR 4 archives (`rar4_*`): rar 6.23 in `cvt-rar4:1` (Ubuntu 22.04), `-ma4`.
The tree commands store the symlink as a link (`-ol`) and recurse (`-r`);
`-idq` only silences the tool. The password is always `secret`.

| File | Content | Command |
| --- | --- | --- |
| `rar5_tree_m0.rar` ... `rar5_tree_m5.rar` | the whole tree, methods 0 (store) to 5 | `rar a -ma5 -m<N> -ol -r -idq rar5_tree_m<N>.rar hello.txt lorem.txt noise.bin empty.txt exec.sh dir unicode long link` |
| `rar5_tree_solid.rar` | the whole tree, solid | `rar a -ma5 -m3 -s -ol -r -idq rar5_tree_solid.rar <same list>` |
| `rar5_dict128k.rar` | lorem.txt, noise.bin, hello.txt; 128 KiB dictionary, solid | `rar a -ma5 -m3 -md128k -s -idq rar5_dict128k.rar lorem.txt noise.bin hello.txt` |
| `rar5_blake2.rar` | hello.txt, lorem.txt, noise.bin with BLAKE2sp hashes | `rar a -ma5 -m3 -htb -idq rar5_blake2.rar hello.txt lorem.txt noise.bin` |
| `rar5_pass.rar` | hello.txt, lorem.txt, AES-256 | `rar a -ma5 -m3 -psecret -idq rar5_pass.rar hello.txt lorem.txt` |
| `rar5_pass_headers.rar` | the same with encrypted headers | `rar a -ma5 -m3 -hpsecret -idq rar5_pass_headers.rar hello.txt lorem.txt` |
| `rar5_comment.rar` | hello.txt and the archive comment | `rar a -ma5 -m3 -z../in/comment.txt -idq rar5_comment.rar hello.txt` |
| `rar5_links.rar` | hello.txt, lorem.txt, `hard.txt` (hard link to hello.txt), `copy.txt` (identical to lorem.txt, stored as a file copy reference) | `ln hello.txt hard.txt; cp lorem.txt copy.txt; touch -d @1623758400 copy.txt hard.txt; rar a -ma5 -m3 -oh -oi -idq rar5_links.rar hello.txt lorem.txt hard.txt copy.txt` |
| `rar5_volume.part1.rar`, `rar5_volume.part2.rar` | hello.txt and noise.bin stored in 2500-byte volumes (noise.bin is split) | `rar a -ma5 -m0 -v2500b -idq rar5_volume.rar hello.txt noise.bin` |
| `rar5_filters.rar` | x86.bin (E8E9 filter), rgb.bmp, pcm.wav, table.dat (DELTA filters with 3, 4 and 8 channels) | `rar a -ma5 -m5 -idq rar5_filters.rar x86.bin rgb.bmp pcm.wav table.dat` (in `in`) |
| `rar5_text.rar` | text.txt | `rar a -ma5 -m5 -idq rar5_text.rar text.txt` (in `in`) |
| `rar5_far.rar` | far.bin (matches up to 2.5 MB back) | `rar a -ma5 -m3 -idq rar5_far.rar far.bin` |
| `rar5_sparse_x86.rar` | sparse.exe (rar 7.01 chose no filter for it) | `rar a -ma5 -m3 -idq rar5_sparse_x86.rar sparse.exe` |
| `rar4_tree_m1.rar` ... `rar4_tree_m5.rar` | the whole tree, methods 1 to 5 | `rar a -ma4 -m<N> -ol -r -idq rar4_tree_m<N>.rar <same list>` |
| `rar4_tree_solid.rar` | the whole tree, solid | `rar a -ma4 -m3 -s -ol -r -idq rar4_tree_solid.rar <same list>` |
| `rar4_pass.rar` | hello.txt, lorem.txt, AES-128 | `rar a -ma4 -m3 -psecret -idq rar4_pass.rar hello.txt lorem.txt` |
| `rar4_pass_headers.rar` | the same with encrypted headers | `rar a -ma4 -m3 -hpsecret -idq rar4_pass_headers.rar hello.txt lorem.txt` |
| `rar4_pass_long.rar` | hello.txt, lorem.txt, AES-128 with the 40-character password `0123456789abcdefghijklmnopqrstuvwxyzABCD` (long enough for RAR's SHA-1 key derivation quirk) | `rar a -ma4 -m3 -p0123456789abcdefghijklmnopqrstuvwxyzABCD -idq rar4_pass_long.rar hello.txt lorem.txt` |
| `rar4_comment.rar` | hello.txt and the archive comment (NEWSUB `CMT` block) | `rar a -ma4 -m3 -z../in/comment.txt -idq rar4_comment.rar hello.txt`, then `python3 fix_cmt_time.py rar4_comment.rar` (rar stamps the comment block with the current time; the script sets it to 2021-06-15 12:00:00 and recomputes that header's CRC) |
| `rar4_volume.part1.rar`, `rar4_volume.part2.rar` | hello.txt and noise.bin stored in 2500-byte volumes | `rar a -ma4 -m0 -v2500b -idq rar4_volume.rar hello.txt noise.bin` |
| `rar4_filter_e8.rar` | x86.bin through the standard E8 VM filter | `rar a -ma4 -m5 -mce+ -idq rar4_filter_e8.rar x86.bin` (in `in`) |
| `rar4_filter_delta.rar` | table.dat through the DELTA VM filter (8 channels) | `rar a -ma4 -m5 -mcd+ -idq rar4_filter_delta.rar table.dat` |
| `rar4_filter_rgb.rar` | rgb.bmp through the RGB VM filter | `rar a -ma4 -m5 -mcc+ -idq rar4_filter_rgb.rar rgb.bmp` |
| `rar4_filter_audio.rar` | pcm.wav through the AUDIO VM filter (4 channels) | `rar a -ma4 -m5 -mca+ -idq rar4_filter_audio.rar pcm.wav` |
| `rar4_text.rar` | text.txt, LZ only | `rar a -ma4 -m5 -mct- -idq rar4_text.rar text.txt` |
| `rar4_sparse_x86.rar` | sparse.exe through ten E8 filter blocks at file offsets up to 589824 | `rar a -ma4 -m3 -mce+ -idq rar4_sparse_x86.rar sparse.exe` |
| `rar4_far.rar` | far.bin (all distance slots up to the 4 MB window) | `rar a -ma4 -m3 -idq rar4_far.rar far.bin` |
| `rar4_wrap.rar` | wrap.bin with a 64 KiB dictionary | `rar a -ma4 -m3 -md64k -idq rar4_wrap.rar wrap.bin` |
| `rar4_ppmd.rar` | text.txt in PPMd blocks | `rar a -ma4 -m5 -mct+ -idq rar4_ppmd.rar text.txt` |

`make_inputs.py` and `fix_cmt_time.py` are the generator and the timestamp
fix mentioned above.

Every archive was checked with `unrar t` / `unrar lta` (all times are
2021-06-15 12:00:00) and scanned for user, host and path data.

## Interop of our output

RAR is read only (RAR's compressor is proprietary), so there is no output of
ours to check. Every fixture was extracted with `unrar` 7.01 during
development, and the extracted files match what the reader produces.

## Gaps

rar 6.23 applied no Itanium filter with `-mci+` and rar 7.01 no ARM filter
to synthetic Itanium / ARM code (with or without an ELF header), so those two
filters have no fixture.
