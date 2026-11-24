# Sources: encoding/unicode

Expected values for `core/tests/families/encoding/unicode_test.cpp`, computed with Python 3.14.10
(Unicode 16.0.0) by `unicode_vectors.py` (kept here, not used by the build or the tests; run it in
this directory to regenerate).

| File | Contents | Python |
| --- | --- | --- |
| `single_code_points.sha256` | for each of NFC, NFD, NFKC, NFKD: the SHA-256 of the concatenated normal forms of every code point U+0000..U+2FFFF (surrogates skipped), each followed by `\n` | `unicodedata.normalize`, `hashlib.sha256` |
| `sequences.txt` | 400 pseudo-random base + combining mark sequences (fixed seed), with their four normal forms, as hex code points separated by `;` | `unicodedata.normalize` |
| `entities.txt` | 34 strings with character references (legacy names without `;`, longest-prefix matches, two-character entities, numeric references) and their decoding | `html.unescape` |
| `idna.txt` | ten internationalised domain names and their ASCII form | the `idna` codec (`str.encode("idna")`, IDNA 2003) |

The normalisation tables themselves (`core/src/families/encoding/unicode_tables.inc`) and the HTML5
entity list (`html_entities.inc`) come from Python's `unicodedata` and `html.entities.html5` through
`core/src/families/encoding/unicode_gen.py`. Our IDNA implementation follows UTS #46 non-transitional
processing, so it differs from the IDNA 2003 codec for `ß` (kept, `xn--fa-hia.de`, instead of `ss`)
and lower-cases ASCII labels; the test accounts for both.
