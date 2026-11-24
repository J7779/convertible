# Sources: encoding/charsets

Sample texts in legacy encodings for `core/tests/families/encoding/charsets_test.cpp`, written with
Python 3.14's codecs by `charsets_fixtures.py` (kept here, not used by the build or the tests; run it
in this directory to regenerate). The texts are made-up sentences and well-known pangrams.

| Files | Contents |
| --- | --- |
| `ja.utf8`, `ja_halfwidth.utf8`, `zh_cn.utf8`, `zh_tw.utf8`, `ko.utf8`, `ru.utf8`, `el.utf8`, `pl.utf8`, `fr.utf8`, `he.utf8` | the reference texts in UTF-8 |
| `<codec>.txt` | one reference text encoded with Python's codec `<codec>` (`str.encode(codec)`): shift_jis, cp932, euc_jp, iso2022_jp, gbk, gb18030, big5, euc_kr, koi8_r, cp1251, cp866, iso8859_5, cp1253, iso8859_7, cp1250, iso8859_2, cp852, mac_roman, cp850, cp437, cp037, cp500, cp1255, iso8859_15 |
| `index.txt` | which reference text each encoded file holds |

The decoding tables themselves (`core/src/util/charset_tables.inc`, shared by every family
through `util/charsets.hpp`) come from Python's codecs through `charsets_gen.py`; IBM-1047, which Python lacks, from glibc 2.39 iconv
(`iconv -f IBM1047 -t UTF-32BE` for every byte, in a throwaway Ubuntu 24.04 container).

Exhaustive check while developing (a script driving our CLI): for all 40 charsets, every character
Python can encode (the whole BMP, plus supplementary samples for GB18030) is encoded by us to exactly
Python's bytes, and Python's bytes decode to exactly Python's text; every byte pair 0x81..0xFF ×
0x30..0xFF (every byte for the single-byte code pages) that Python decodes validly, we decode to the
same character. The only differences are in error recovery: for an invalid sequence we sometimes emit
one U+FFFD where Python emits two (or the reverse), and we decode a lone 0x80 in GBK as the euro sign
(code page 936), which Python's `gbk` rejects. EUC-KR's eight-byte composed-syllable sequences are not
supported (U+3164 HANGUL FILLER is written as A4 D4, as Python does).
