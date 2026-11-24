# HEIF / AVIF / HEIC fixtures

Every picture is a synthetic test card from `make_sources.py` (standard
library only): colour ramps `r = 255x/(w-1)`, `g = 255y/(h-1)`,
`b = 255(x+y)/(w+h-2)`, inverted on every 8x8 block where
`(x/8 + y/8) % 5 == 0`, plus a 6x6 yellow square at `(w/3, h/4)`. Variants: an
RGBA card with a radial alpha and a fully transparent 6x6 corner, a 16-bit RGB
ramp, the card with a small hand-made matrix/TRC ICC profile ("Test RGB", no
copyright), three horizontally shifted 32x24 frames, and the 80x48 card
point-sampled to 1030x520. The tests regenerate the card in C++ to compare
with the source pattern.

`make_fixtures.sh` regenerates every file (run it from an empty scratch
directory: `sh <this-directory>/make_fixtures.sh <this-directory>`).
`add_exif.py` inserts a synthetic EXIF block (Orientation 6, 72 dpi, Software
"TestPattern") into a JPEG.

Tools: libheif 1.23.7 `heif-enc`/`heif-dec` with the aom 3.15.3 (AV1) and
x265 4.3 (HEVC) plug-ins, libavif 1.0.4 `avifenc`/`avifdec` (Ubuntu 24.04
package, in a throwaway container), macOS 27.2 `sips` (Apple's HEIC/AVIF encoder),
ImageMagick 7.1.2 (only to write the intermediate JPEG and to crop one
reference PNG, with `-strip`). No file holds personal or machine data; the
only text in them is the pattern's "TestPattern" EXIF/XMP and the ICC
description.

## Files

| File | Content | Command |
| --- | --- | --- |
| `avif_420.avif` | 64x48 card, AV1 8-bit 4:2:0 | `heif-enc -A -q 75 -o avif_420.avif src/card_64x48.png` |
| `avif_444_10bit.avif` | 48x32 16-bit ramp, AV1 10-bit 4:4:4 | `heif-enc -A -q 85 -p chroma=444 -b 10 -o avif_444_10bit.avif src/ramp16_48x32.png` |
| `avif_lossless.avif` | 33x21 card, lossless (identity matrix, 4:4:4) | `heif-enc -A -L -o avif_lossless.avif src/card_33x21.png` |
| `avif_alpha.avif` | 48x32 RGBA card, alpha auxiliary item (`urn:mpeg:mpegB:cicp:systems:auxiliary:alpha`) | `heif-enc -A -q 80 -o avif_alpha.avif src/alpha_48x32.png` |
| `avif_grid.avif` | 80x48 card as a 3x2 `grid` of 32x32 AV1 tiles | `heif-enc -A -q 80 --cut-tiles 32 -o avif_grid.avif src/card_80x48.png` |
| `avif_sequence.avif` | image sequence (`avis`, `moov` track), three 32x24 frames of 100 ms | `heif-enc -A -q 70 -S --timebase 1000 --duration 100 -o avif_sequence.avif src/seq_1.png src/seq_2.png src/seq_3.png` |
| `avif_pq_10bit.avif` | the 16-bit ramp as BT.2020 primaries, PQ transfer, BT.2020 matrix, 10 bit | `heif-enc -A -q 85 -b 10 --colour_primaries 9 --transfer_characteristic 16 --matrix_coefficients 9 -o avif_pq_10bit.avif src/ramp16_48x32.png` |
| `avif_sips.avif` | 64x48 card, Apple's AVIF encoder | `sips -s format avif src/card_64x48.png --out avif_sips.avif` |
| `heic_420.heic` | 64x48 card, HEVC 8-bit 4:2:0 (coded 64x64 with a `clap` crop) | `heif-enc -q 75 -o heic_420.heic src/card_64x48.png` |
| `heic_10bit.heic` | 48x32 16-bit ramp, HEVC Main 10 | `heif-enc -q 85 -b 10 -o heic_10bit.heic src/ramp16_48x32.png` |
| `heic_grid.heic` | 80x48 card as a 3x2 grid of HEVC tiles (each tile coded 64x64 with `clap` 32x32) | `heif-enc -q 80 --cut-tiles 32 -o heic_grid.heic src/card_80x48.png` |
| `heic_alpha.heic` | 48x32 RGBA card, HEVC alpha auxiliary item | `heif-enc -q 80 -o heic_alpha.heic src/alpha_48x32.png` |
| `heic_irot_imir.heic` | 64x48 card stored with `irot` and `imir` (displayed 48x64) | `heif-enc -q 80 --rotate-cw 90 --flip-h -o heic_irot_imir.heic src/card_64x48.png` |
| `heic_thumb.heic` | 33x21 card plus a 16-pixel `thmb` thumbnail item | `heif-enc -q 80 -t 16 -o heic_thumb.heic src/card_33x21.png` |
| `heic_icc.heic` | 32x24 card with an ICC `colr` ('prof') property | `heif-enc -q 80 -o heic_icc.heic src/card_icc_32x24.png` |
| `heic_sips.heic` | 64x48 card, Apple's HEIC encoder (nclx with unspecified primaries/transfer, `clli`) | `sips -s format heic -s formatOptions 80 src/card_64x48.png --out heic_sips.heic` |
| `heic_sips_grid.heic` | 1030x520 card, Apple's iPhone-style 3x2 grid of 512x512 tiles | `sips -s format heic -s formatOptions 50 src/card_1030x520.png --out heic_sips_grid.heic` |
| `heic_sips_10bit.heic` | 48x32 16-bit ramp, Apple's HEVC Main 10 | `sips -s format heic src/ramp16_48x32.png --out heic_sips_10bit.heic` |
| `heic_sips_alpha.heic` | 48x32 RGBA card, Apple's alpha item (`urn:mpeg:hevc:2015:auxid:1`) | `sips -s format heic src/alpha_48x32.png --out heic_sips_alpha.heic` |
| `heic_sips_exif6.heic` | the 64x48 card from a JPEG with EXIF orientation 6: Apple stores the pixels unrotated with `irot` 270 (anticlockwise) AND keeps EXIF orientation 6, plus an XMP item | `magick src/card_64x48.png -strip -quality 95 src/card_64x48.jpg`; `python3 add_exif.py src/card_64x48.jpg src/card_exif6.jpg 6`; `sips -s format heic src/card_exif6.jpg --out heic_sips_exif6.heic` |
| `ref_*.png` | libheif's decode of each file above (16-bit PNG for the 10-bit files; the PQ file as PQ code values) | `heif-dec <file> ref_<name>.png` |
| `ref_heic_sips_grid_crop.png` | 160x120 window at (440, 400) of libheif's decode of `heic_sips_grid.heic` | `heif-dec heic_sips_grid.heic ref_heic_sips_grid.png`; `magick ref_heic_sips_grid.png -crop 160x120+440+400 +repage -strip ref_heic_sips_grid_crop.png` |

| `card_64x48.jpg` | the 64x48 card as a baseline JPEG, wrapped by the tests into a hand-made HEIF 'jpeg' item | `magick src/card_64x48.png -strip -quality 95 src/card_64x48.jpg` |
| `avif_anim_avifenc.avif` | libavif's animated AVIF (4:4:4, edit list, three 100 ms frames of `seq_*.png`) | `avifenc -q 70 --timescale 1000 --duration 100 -s 8 src/seq_1.png src/seq_2.png src/seq_3.png avif_anim_avifenc.avif` (libavif 1.0.4, Ubuntu 24.04 `libavif-bin`, in a container) |
| `avifdec_avif_anim_avifenc_1.png` | libavif's decode of the second frame | `avifdec --index 1 avif_anim_avifenc.avif avifdec_avif_anim_avifenc_1.png` |
| `avifdec_avif_420.png`, `avifdec_avif_alpha.png`, `avifdec_avif_sips.png` | libavif's decodes (4:2:0 chroma interpolated) | `avifdec <file>.avif avifdec_<file>.png` |

## What the tests check

- With `chroma_upsampling=nearest` (what libheif does) every file decodes to
  libheif's result (PSNR >= 38 dB, SSIM >= 0.98 for 8 bit; >= 42-45 dB for
  10 bit; in practice 60+ dB); with the default bilinear upsampling the AVIF
  files match libavif's avifdec (>= 45 dB, in practice 52+ dB). Against the
  source card: >= 27.5 dB at the encoders' quality settings (hard 8x8 block
  edges in 4:2:0).
- `avif_lossless.avif` is bit-exact against both libheif and the source.
- The sips grid matches libheif's window across the tile seams (>= 38 dB).
- 10-bit files keep their precision in `ir::Image::hdr` (source bit depth 10);
  the PQ file becomes linear BT.709 light (1.0 = 203 nits) within 3 % of the
  values computed independently from libheif's PQ codes, and a tone-mapped frame.
- `heic_sips_exif6.heic` is upright after `irot` alone (EXIF orientation not
  applied twice) and its kept EXIF block says orientation 1.
- The thumbnail item is ignored; the ICC profile is returned verbatim; both
  sequences give three 100 ms frames matching the shifted source cards.

## Interoperability of our encoders

Checked while developing by converting the source PNGs with the `convertible`
CLI and opening the results with libheif 1.23.7 `heif-dec`, libavif 1.0.4
`avifdec` (container) and macOS 27.2 `sips`; PSNR against the source with
`magick compare -metric PSNR`:

| Output | libheif | avifdec | sips |
| --- | --- | --- | --- |
| AVIF q90 4:2:0, 64x48 card | 32.4 dB | 27.3 dB | 25.0 dB |
| AVIF q50 | 29.3 dB | 26.2 dB | 24.3 dB |
| AVIF q90 4:4:4 | 43.7 dB | 43.7 dB | 43.2 dB |
| AVIF lossless (identity matrix) | exact | exact | exact |
| AVIF 10 bit (16-bit ramp source) | 40.8 dB | 48.2 dB | 40.8 dB |
| AVIF with alpha | 41.7 dB | 31.2 dB | 28.9 dB |
| AVIF 33x21 (odd size) | 23.5 dB | 24.6 dB | 23.5 dB |
| HEIC q90, HEIF (mif1) q90 | 32.3 dB | - | 25.1 dB |
| HEIC 33x21 (coded 34x22 + clap) | 23.6 dB | - | 23.6 dB |
| HEIC 16900x48 (512x512 tile grid) | 39.1 dB | - | 27.5 dB |
| HEIC with EXIF/XMP kept (`strip_metadata=false`) | opens, EXIF + XMP items, orientation 1 | - | opens |

The spread between decoders on the 4:2:0 card comes from their chroma
upsampling (libheif repeats samples, libavif and Apple interpolate); libheif's
and Apple's own files show the same spread (sips vs libheif about 25.5 dB on
`heic_sips.heic`). HEIC alpha items written with `keep_alpha=true` are 4:2:0
HEVC pictures (the HEVC encoder has no 4:0:0 mode): libheif and our reader
show them, but sips cannot open such files, so HEIC transparency is blended
onto the background by default.
