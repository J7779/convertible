# JPEG XL fixtures

All pictures are synthetic test patterns (gradients, coloured rectangles,
checkerboards, deterministic pseudo-random grain, the 16-bit formula
`(x * 2039 + y * 977 + c * 12345) % 65536`). The only metadata is the made-up
EXIF/XMP written by `make_sources.py` (ImageDescription "Test pattern",
Software "Convertible test", dc:title "Test pattern").

## How they were made

1. `python3 make_sources.py` (Python 3 standard library only) writes the source
   pictures: `src_rgb.png` (64x48 RGB), `src_rgba.png` (RGBA with an alpha ramp
   and a fully transparent band), `src_grey.png` (8-bit grey), `src_rgb16.png`
   (40x30, 16-bit RGB), `src_float.pfm` (32x24 float RGB, values up to 4.0),
   `src_exif.png` (RGB with `eXIf` + XMP `iTXt`), `src_orient6.png` (40x24 with a
   red top-left marker and EXIF orientation 6), `src_anim.png` (APNG, 3 frames of
   32x32, delays 100/200/300 ms, infinite loop) and `src_icc.png` (RGB tagged with
   a synthetic ICC v2 profile whose tone curves are 16-entry sampled tables, so it
   cannot be expressed as a JPEG XL colour enum).
2. `make_fixtures.sh`, run in a throwaway container:
   `docker run --rm --memory 2g -v "$PWD":/w --workdir /w debian:trixie sh make_fixtures.sh`
   installs Debian's `libjxl-tools` (cjxl/djxl/jxlinfo v0.11.2) and
   `libjpeg-turbo-progs`, then runs exactly these commands:

| File | Command |
| --- | --- |
| `lossless_rgb.jxl` | `cjxl src_rgb.png lossless_rgb.jxl -d 0 -e 7` |
| `lossless_rgba.jxl` | `cjxl src_rgba.png lossless_rgba.jxl -d 0 -e 7` |
| `lossless_grey.jxl` | `cjxl src_grey.png lossless_grey.jxl -d 0 -e 7` |
| `lossless_rgb16.jxl` | `cjxl src_rgb16.png lossless_rgb16.jxl -d 0 -e 7` |
| `lossless_float.jxl` | `cjxl src_float.pfm lossless_float.jxl -d 0 -e 7` (cjxl tags PFM input as sRGB-encoded) |
| `anim.jxl` | `cjxl src_anim.png anim.jxl -d 0 -e 7` |
| `icc_lossless.jxl` | `cjxl src_icc.png icc_lossless.jxl -d 0 -e 7` (ICC profile stored, no colour enum) |
| `orient6.jxl` | `cjxl src_orient6.png orient6.jxl -d 0 -e 7` (codestream orientation 6 + Exif box) |
| `exif_boxes.jxl` | `cjxl src_exif.png exif_boxes.jxl -d 0 -e 7 --compress_boxes=1` (Brotli `brob` Exif + XMP boxes) |
| `exif_plain.jxl` | `cjxl src_exif.png exif_plain.jxl -d 0 -e 7 --compress_boxes=0` (plain `Exif` + `xml ` boxes) |
| `lossy_d1.jxl` | `cjxl src_rgb.png lossy_d1.jxl -d 1 -e 7` |
| `lossy_d3.jxl` | `cjxl src_rgb.png lossy_d3.jxl -d 3 -e 7` |
| `lossy_modular_d2.jxl` | `cjxl src_rgb.png lossy_modular_d2.jxl -d 2 -m 1 -e 7` |
| `lossy_rgba_d1.jxl` | `cjxl src_rgba.png lossy_rgba_d1.jxl -d 1 -e 7` |
| `src_jpeg.jpg` | `cjpeg -quality 85 -outfile src_jpeg.jpg src_rgb.ppm` (PPM copy of `src_rgb.png`, deleted afterwards) |
| `jpeg_recompressed.jxl` | `cjxl src_jpeg.jpg jpeg_recompressed.jxl --lossless_jpeg=1` (`jbrd` box; `djxl` restores `src_jpeg.jpg` byte for byte) |
| `*.ref.png` | `djxl <name>.jxl <name>.ref.png` for the lossy files and `jpeg_recompressed.jxl` |

All `cjxl`/`djxl` calls also pass `--quiet`.

## What the tests check

- Lossless files decode bit-exactly against the source PNGs (RGB, RGBA, grey,
  16-bit via `hdr`, the ICC-tagged picture together with its profile bytes, all
  three animation frames with their delays).
- Float: the linearised `hdr` values match the PFM values within 0.2 % up to
  1.5, and within 3 % above (libjxl's own approximation of the sRGB curve
  drifts for very bright values: 1.6 % at 4.0).
- Lossy files against djxl's own decode: PSNR > 48 dB and max difference
  <= 2 (measured: 53.2 dB d1, 53.3 dB d3, 53.2 dB modular d2, 59.8 dB RGBA d1,
  53.1 dB recompressed JPEG). Against the noisy source picture: d1 30.3 dB,
  d3 28.7 dB, modular d2 24.2 dB (thresholds 29.5 / 28 / 23.5).
- Orientation 6 is applied to the pixels; EXIF/XMP are read from plain and
  Brotli-compressed boxes.

## Interoperability of our encoder

Our writer's output (CLI `convertible convert ... -t jxl`, from the sources
above) was checked in the same container with libjxl 0.11.2's `jxlinfo` and
`djxl` plus ImageMagick `compare`:

- `jxlinfo` reports the expected structure for every file: lossless RGB, RGBA,
  greyscale (1 channel), 16-bit, 32-bit float with linear transfer function,
  3-frame animation with 100/200/300 ms, plain `Exif` and `xml ` boxes when
  `strip_metadata=false`, the 584-byte ICC profile, lossy VarDCT.
- `djxl` decodes all of them. Lossless RGB/RGBA/grey/16-bit/ICC/EXIF outputs are
  identical to the sources (ImageMagick AE 0); the float file decodes to the
  exact PFM values. Lossy against the source: quality 90 30.4 dB (1350 bytes),
  50 26.9 dB (469 bytes), 20 23.6 dB (284 bytes); effort 1 28.7 dB, effort 9
  30.5 dB; RGBA quality 90 36.4 dB. (The source has +-4 grain, which lossy
  coding smooths away, so these PSNRs are low by design.)
