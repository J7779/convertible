# Sources: subtitle

Fixtures for `core/tests/families/subtitle/`. All content is synthetic: a
made-up script ("Ada Example", "Ben"), accented Latin, Hebrew and Arabic
(right-to-left), an emoji, a long line, two overlapping cues and a cue after
one hour. Commands are run in a scratch directory outside the repository; the
generator scripts are reproduced in full or summarised below.

Tools: pysubs2 1.9.1 and webvtt-py 0.8.0 in a Python 3.14 virtual
environment, ffmpeg 6.1.1 (Ubuntu 24.04 package) in a throw-away Docker
container, and the Python 3.14 standard library for formats that have no
widely used writer.

## The master script

| # | Start | End | Text |
| --- | --- | --- | --- |
| 1 | 00:00:01.000 | 00:00:03.500 | `Hello, Ada Example.` (speaker Ada) |
| 2 | 00:00:04.000 | 00:00:06.250 | `Café déjà vu` in italics, line break, `second line` (Ada) |
| 3 | 00:00:06.500 | 00:00:09.000 | `שלום עולם — مرحبا بالعالم` (Ben) |
| 4 | 00:00:09.100 | 00:00:12.345 | `Emoji test 🎬 and bold words`, "bold" in bold (Ben) |
| 5 | 00:00:12.000 | 00:00:14.000 | `Overlapping cue` (overlaps #4) |
| 6 | 00:00:15.000 | 00:00:20.000 | an 89-character line |
| 7 | 00:00:21.000 | 00:00:23.000 | `Positioned at the top` (style `Top`, alignment 8, `\pos(960,100)` on 1920x1080) |
| 8 | 01:00:01.000 | 01:00:03.500 | `One hour later` |

## Files written by pysubs2

`gen_subs.py` builds the script above as a `pysubs2.SSAFile` (title
"Fixture script", PlayRes 1920x1080, an extra style `Top`) and saves it:

```
python -I gen_subs.py out
```

| File | Saved as | Notes |
| --- | --- | --- |
| `basic_pysubs2.ass` | `subs.save("pysubs2.ass")` | ASS v4.00+, the reference |
| `basic_pysubs2.ssa` | `subs.save("pysubs2.ssa", format_="ssa")` | SSA v4.00, legacy alignment 6 for `Top`, decimal colours |
| `basic_pysubs2.srt` | `subs.save("pysubs2.srt")` | pysubs2 keeps italics, drops bold and position |
| `basic_pysubs2.vtt` | `subs.save("pysubs2.vtt", format_="vtt")` | numeric cue identifiers |
| `microdvd_25fps_pysubs2.sub` | `subs.save("pysubs2_25fps.sub", format_="microdvd", fps=25)` | `{1}{1}25` header, frame numbers rounded down |
| `tmplayer_pysubs2.txt` | `subs.save("pysubs2.tmp.txt", format_="tmp")` | whole-second start times only |
| `mpl2_pysubs2.txt` | `subs.save("pysubs2.mpl2.txt", format_="mpl2")` | deciseconds, truncated |

## Files written by ffmpeg

From `basic_pysubs2.srt`, in `docker run --rm --memory 2g -v "$PWD/out":/w -w /w <ubuntu 24.04 + ffmpeg 6.1.1>`:

| File | Command |
| --- | --- |
| `basic_ffmpeg.ass` | `ffmpeg -i pysubs2.srt -map_metadata -1 -fflags +bitexact ffmpeg.ass` |
| `basic_ffmpeg.vtt` | `ffmpeg -i pysubs2.srt -map_metadata -1 -fflags +bitexact ffmpeg.vtt` (mm:ss.ttt time stamps) |
| `basic_ffmpeg.lrc` | `ffmpeg -i pysubs2.srt -map_metadata -1 -fflags +bitexact ffmpeg.lrc` (start times only; the two lines of cue 2 share one stamp) |

## Hand-written files (Python standard library)

`gen_hand.py` writes each file below with `open(..., "wb").write(text.encode(...))`;
`python3 -I gen_hand.py hand`. Contents:

| File | Contents |
| --- | --- |
| `messy.srt` | UTF-8 BOM, CRLF; cue 1 with `X1:.. Y2:..` coordinates after the end time, `{\an8}` and `<font color>`; cue 2 without index, `00:00:04.000` (dot), `<I>` upper-case and an unclosed `<b>`; cue 3 `0:0:7,5 --> 0:0:9,25` (one-digit fields) and `&amp;`; cue 4 `-->` without spaces |
| `cp1251.srt` | two Russian cues encoded `cp1251` |
| `cp1250.srt` | Czech and Polish pangrams encoded `cp1250` |
| `cp1253.srt` | one Greek cue encoded `cp1253` |
| `cp1252.srt` | one French cue encoded `cp1252` |
| `utf16le.srt` | one cue encoded `utf-16` (little endian with BOM) |
| `features.vtt` | WebVTT header text and `Kind:`/`Language:` lines, a `REGION`, a `STYLE`, a two-line `NOTE`, cue identifier `intro` with `align:start line:10% position:20%`, voices `<v Ada Example>` and `<v.first Ben>`, a class span, `&amp; &lt; &gt;`, ruby with `<rt>`, karaoke time stamps, short `mm:ss.ttt` times |
| `youtube.sbv` | three SBV cues (`h:mm:ss.mmm,h:mm:ss.mmm`) |
| `subviewer2.sub` | SubViewer 2.0 with `[INFORMATION]` header, `[br]` line break, CRLF |
| `subviewer1.sub` | SubViewer 1.0 (`**START SCRIPT**`, `[hh:mm:ss]` start and end lines) |
| `quicktime.txt` | QuickTime Text with `{timeScale:30}`: `[00:00:03.15]` = 3.5 s, `[00:00:06.06]` = 6.2 s |
| `spruce.stl` | Spruce STL with `$` settings, `//` comments, `00:00:03:12` frame time codes and `^I` italics |
| `enhanced.lrc` | LRC with `[ti:] [ar:] [al:] [offset:+500] [length:01:00]`, a line with two stamps, enhanced `<mm:ss.xx>` word stamps and an empty end line |

## Interoperability checks (development, not part of the test run)

The hand-written files were cross-checked with independent readers:
`ffmpeg -i <file> -f srt -` read `messy.srt`, `subviewer2.sub`, `spruce.stl`
and `enhanced.lrc`; webvtt-py read `features.vtt` (same voices and texts),
`youtube.sbv` and pysubs2 read `messy.srt`. Known differences: ffmpeg reads
`0:0:7,5` as 7.005 s while pysubs2 and Convertible read 7.5 s; ffmpeg's
Spruce reader treats the last time-code field as centiseconds (Convertible
reads frames, `fps` option, default 25; `{"fps":100}` reproduces ffmpeg).

Files written by Convertible from `basic_pysubs2.ass` (`convertible convert
out/pysubs2.ass -t <format>`) were read back with independent software and
matched the script (times to the format's resolution, text and styles):

- pysubs2: srt (incl. `{\an8}`), ass (styles, speakers, `\pos`), ssa, sbv
  (via webvtt-py), mpl2, tmplayer (pysubs2 warns about the empty end-of-cue
  lines but reads every cue);
- webvtt-py: vtt (voices become speakers);
- ffmpeg 6.1.1: vtt, ass, ssa, microdvd (23.976 fps header honoured),
  subviewer, lrc.

## Broadcast and XML formats

`gen_broadcast.py` (`python -I gen_broadcast.py bc`, pycaption 2.3.17 and
the Python standard library) reads this SRT with pycaption's `SRTReader`:

```
00:00:01,000 --> 00:00:03,500  Hello, Ada Example.
00:00:04,000 --> 00:00:06,250  <i>Café déjà vu</i> / second line
00:00:07,000 --> 00:00:09,000  Plain caption text
00:00:10,000 --> 00:00:12,500  Ünïcödé and ½ ♪ ©
```

| File | Tool | Contents |
| --- | --- | --- |
| `pycaption.dfxp` | pycaption `DFXPWriter().write(captions)` | DFXP; pycaption keeps the SRT `<i>` as escaped text |
| `pycaption.smi` | pycaption `SAMIWriter().write(captions)` | SAMI, class `en` |
| `pycaption.scc` | pycaption `SCCWriter().write(captions)` | pop-on CEA-608, non-drop time codes, special (½ ♪) and extended (Ü ï ö ©) characters |
| `timing_forms.ttml` | hand-written | TTML with clock, frame (`ttp:frameRate="25"`, `subFrameRate="2"`), offset (`s`, `ms`, `f`, `h`) and tick (`tickRate="10000000"`) times, `div` begin offsets, chained style references, regions with `displayAlign`/`textAlign`, `<br/>`, `ttm:title` |
| `ebu_tt_d.xml` | hand-written | EBU-TT-D (namespaces `urn:ebu:tt:*`), German text, italic style |
| `smpte_tt.xml` | hand-written | SMPTE-TT, `ttp:frameRate="30"` with multiplier 1000/1001, SMPTE time codes |
| `youtube.srv3` | hand-written | YouTube `timedtext format="3"` with `<s>` word segments and an append window |
| `youtube.json3` | hand-written | YouTube json3 with a window event, word segments and an `aAppend` event |
| `rollup_painton.scc` | hand-written (`struct`-free hex, odd parity computed in the script) | CC1 RU2 roll-up (three lines, CR), EDM, RDC paint-on with an italics PAC, drop-frame time codes |
| `handmade.stl` | hand-written with `struct.pack` | EBU STL: GSI (CPN 850, STL25.01, CCT 00, LC 09, title, TCP 10:00:00:00), five TTI blocks (ISO 6937 diacritics, 0x80/0x81 italics, 0x82/0x83 underline, 0x8A line breaks, teletext controls, VP/JC positions, one comment block) |

Interoperability (development): ttconv 1.2.3 (`tt convert -i handmade.stl
--itype stl --otype srt`) reads `handmade.stl` with the same times and texts.
pycaption's `SCCReader` reads `rollup_painton.scc` as the four captions the
tests expect, and `pycaption.scc` with its own start times up to 100 ms early.

Files written by Convertible from the SRT above were read back:
- pycaption `SCCReader`, `SAMIReader`, `DFXPReader`: scc, sami, dfxp and ttml
  outputs give the four captions with identical times (SCC within one frame)
  and texts;
- ttconv 1.2.3 (`tt convert ... --otype srt`): ttml, ttml with
  `ttml_profile=imsc1`, ebu-tt, ebu-stl and scc outputs give identical texts
  (including italics) and times within one frame.
