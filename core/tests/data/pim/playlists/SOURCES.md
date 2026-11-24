# Sources: pim/playlists

Written by `gen_pim_b.py` (Python 3.14 standard library, m3u8 6.0.0) in a
scratch virtual environment: `python -I gen_pim_b.py out`. Every playlist
holds the same three synthetic entries:

| # | Location | Title | Artist | Album | Duration |
| --- | --- | --- | --- | --- | --- |
| 1 | `music/01 Überture.flac` | Überture | Ada Example | Synthetic Sessions | 183.5 s |
| 2 | `music/02 שיר.mp3` | שיר בדיוני | Ben Sample | Synthetic Sessions | 241 s |
| 3 | `https://radio.example.org/stream` | Example Radio 🎵 | | | none (stream; group `Live`, `tvg-id="radio1"`) |

| File | Tool | Notes |
| --- | --- | --- |
| `extended.m3u8` | string formatting | `#EXTM3U`, `#PLAYLIST`, `#EXTINF` with attributes, `#EXTALB`, `#EXTART` |
| `hls_media.m3u8` | m3u8 `M3U8().dumps()` | version 3, target duration 10, media sequence 7, three segments, a discontinuity, `#EXT-X-ENDLIST` |
| `hls_master.m3u8` | m3u8 `M3U8().dumps()` | two variants with BANDWIDTH, RESOLUTION, CODECS |
| `configparser.pls` | `configparser.ConfigParser.write` | `[playlist]`, File/Title/Length, NumberOfEntries, Version=2 (lengths rounded) |
| `etree.xspf` | `xml.etree.ElementTree` | XSPF 1 with title, creator, album, trackNum, duration in ms |
| `etree.asx` | `xml.etree.ElementTree` | ASX 3.0 entries with title, author, ref, duration |
| `etree.wpl` | `xml.etree.ElementTree` | WPL / SMIL with `<media src>` only |
| `itunes_library.xml` | `plistlib.dumps(..., fmt=FMT_XML)` | two tracks (Name, Artist, Album, Genre, Total Time, Track Number, file:// Location) and playlist "Favourites" with track 102 |
| `album.cue` | string formatting | REM GENRE/DATE, PERFORMER, TITLE, one FILE with three tracks, INDEX 00 and 01 in mm:ss:ff |

Interoperability (development): playlists written by Convertible
(`convertible convert ... -t m3u8|pls|xspf`) were read back with the m3u8
library (HLS media: target duration, sequence, segments and discontinuity
identical; master: both variants with bandwidths; extended M3U: locations
and durations), `configparser` (PLS) and `xml.etree` (XSPF titles).
