# Test fixtures

Real files that the format tests decode. Load them with
`core/tests/support/fixtures.hpp`:

```cpp
#include "support/fixtures.hpp"

const cvt::Bytes png = cvt::test::fixture("image/gradient_8x8.png");   // bytes
const std::string csv = cvt::test::fixture_text("table/quoted.csv");   // same, as text
```

Names are relative to this directory, UTF-8, with `/` separators. A missing
fixture fails the calling test (it is never skipped), so commit the file
together with the test.

## Layout

```
core/tests/data/<family>/<name>         one directory per format family
core/tests/data/<family>/SOURCES.md     provenance of every file in it
```

`<family>` matches `core/src/families/<family>/`. Use lower-case ASCII file
names that say what is inside (`sine_440hz_mono_8k.wav`, `two_sheets.xlsx`).
Sub-directories are allowed when a family has many files.

## What may be committed

1. **Made by independent software.** A fixture for a decode test is a real
   file written by a widely used program other than Convertible (ffmpeg,
   ImageMagick, libpng tools, LibreOffice, Python's standard library, `zip`,
   `tar`, ...). Never use a file produced by Convertible's own encoders: it
   would only prove that the reader and the writer agree with each other.
2. **Tiny.** Ideally under 50 KB each: 8x8 to 64x64 images, well under a
   second of audio, a few video frames, one-page documents.
3. **Synthetic.** Test patterns, gradients, tones, sweeps, lorem ipsum,
   made-up tables. No photographs, recordings or documents of real things.
4. **Free of personal and machine data.** No user names, real names, host
   names, home or other absolute paths, e-mail addresses, device, GPS or
   other EXIF data, software serial numbers, uid/gid/uname/gname in tar
   headers, author / last-modified-by / company fields in documents, or
   embedded thumbnails and previews of other content. Strip such metadata
   when generating the file (for example `ffmpeg -map_metadata -1 -bitexact`,
   `exiftool -all=`, `zip -X`, `tar --owner=0 --group=0 --numeric-owner`) and
   check the result, e.g. with `strings -a <file>`, before committing.
5. **Yours to give.** Only content you created yourself, contributed under
   CC0-style terms (no rights reserved). Nothing downloaded, nothing from
   another project's test suite.
6. **Committed as-is.** Fixtures are bytes: no re-encoding, no formatting, no
   line-ending conversion. `core/tests/data/.gitattributes` (`* -text`)
   makes git leave everything below this directory untouched.

## SOURCES.md

Every family directory has a `SOURCES.md` with one entry per file:

- what the file contains (dimensions, sample rate, duration, text, and the
  known pixel / sample / cell values the tests rely on);
- the tool and its version that produced it;
- the exact command line, with relative paths only, so that anyone can
  regenerate the file.

See `support/SOURCES.md` for the format. A fixture without an entry does not
pass review.

## The three kinds of tests every format needs

Every format gets tests that convert real files and check the quality of the
result, using `core/tests/support/metrics.hpp` (the header lists starting
thresholds for each metric):

1. **Decode a fixture.** Read a file from this directory, written by
   independent software, and compare the result with its known content:
   exact pixels for lossless images (`pixels_equal`), PSNR / SSIM against the
   same pattern from a generator for lossy ones, sample values and SNR for
   audio, text for documents (`text_similarity`), cells for tables
   (`tables_equal`), values for data (`data_equal`).
2. **Round trip with thresholds.** Encode a generated input, decode the
   output and assert a metric threshold for each quality level the writer
   offers (lossless formats: exact equality). Also assert that a lower
   quality setting really produces a smaller file than a higher one.
3. **Reject damaged input.** Truncated and corrupted files must return an
   error and must never crash, hang or read out of bounds
   (`cvt::test::fuzz_reader`, plus explicit checks that specific broken
   inputs yield an error status). These tests run under ASan/UBSan.
