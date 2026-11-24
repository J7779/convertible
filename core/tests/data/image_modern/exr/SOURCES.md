# OpenEXR fixtures

Every file is written by the reference OpenEXR library, never by
Convertible:

- `make_fixtures.py` (this directory) with the OpenEXR Python bindings
  3.5.5 (`pip install OpenEXR==3.5.5 numpy` in a scratch venv), run from this
  directory: `python make_fixtures.py`.
- `docker_fixtures.sh` in a throwaway `ubuntu:24.04` container
  (`docker run --rm --memory 2g -v "$PWD":/w ubuntu:24.04 sh /w/docker_fixtures.sh`)
  with the distribution's OpenEXR 3.1.5: it builds `make_yca.cc` against
  libOpenEXR (`RgbaOutputFile`, `WRITE_YC`) and runs `exrmaketiled`.

All content is synthetic; the headers hold only the standard attributes
(no names, hosts, dates or software tags).

Patterns (x, y are pixel coordinates; values are exact in half precision):

- **pattern**, 37x23: R = x/8, G = y/4, B = 0.25 + (x+y)/64.
- **alpha**: A = ((x+y) mod 4 + 1)/4; colour is stored premultiplied (R*A ...).
- **busy**, 37x23: R = ((7919x + 104729y) mod 1000)/250, G = (R + x/16) mod 3,
  B = 0.5 + sin(x/3) cos(y/2).
- **float**, 37x23, 32-bit: R = x/3, G = 0.1y, B = 1000.5 + x.
- **uint**, 37x23, 32-bit unsigned: R = 1000x, G = y, B = 70000 + xy.

`*.ref` files hold what the reference library (Python bindings 3.5.5)
decodes from the lossy file of the same name: little-endian half floats,
channels interleaved in the listed order, rows top to bottom.

| File | Content | Tool |
| --- | --- | --- |
| `rgb_half_none.exr`, `_rle`, `_zips`, `_zip`, `_piz`, `_pxr24` | pattern, R/G/B half, one file per lossless compression | make_fixtures.py |
| `rgb_half_b44.exr`, `rgb_half_b44a.exr` (+ `.ref`) | pattern, B44 / B44A | make_fixtures.py |
| `rgb_half_dwaa.exr`, `rgb_half_dwab.exr` (+ `.ref`) | pattern, DWAA / DWAB (level 45) | make_fixtures.py |
| `noise_b44.exr` (+ `.ref`) | busy, half, B44 | make_fixtures.py |
| `noise_float_dwab.exr` (+ `.ref`) | busy, 32-bit float channels, DWAB | make_fixtures.py |
| `rgba_half_piz.exr` | pattern with alpha (premultiplied), PIZ | make_fixtures.py |
| `rgbaz_dwaa.exr` (+ `.ref`, R G B A) | pattern with alpha plus float Z = 0.5x + y, DWAA (lossy colour, RLE alpha, zlib Z) | make_fixtures.py |
| `rgb_float_zip.exr`, `rgb_float_piz.exr`, `rgb_float_pxr24.exr` | float, ZIP / PIZ / PXR24 (24-bit floats) | make_fixtures.py |
| `rgb_uint_rle.exr` | uint, RLE | make_fixtures.py |
| `tiled_zip.exr` | pattern, tiled 16x8, one level, ZIP | make_fixtures.py |
| `decreasing_zip.exr` | pattern, line order DECREASING_Y | make_fixtures.py |
| `datawindow_zip.exr` | display window (0,0)-(39,29), data window (5,3)-(24,20) holding pattern at absolute coordinates | make_fixtures.py |
| `ya_zip.exr` | Y = x/8 + y/16 with alpha (premultiplied) | make_fixtures.py |
| `layers_zip.exr` | pattern in `diffuse.R/G/B` plus float `Z` = y | make_fixtures.py |
| `multipart.exr` | 3 parts sharing a 37x23 display window: "left" pattern (ZIP), "right" with R = 4.5 - x/8 (PIZ), "patch" Y = 0.75 in data window (10,10)-(13,13) | make_fixtures.py |
| `aces_zip.exr` | 4x2: AP0 red, green, blue, grey 0.5 columns; `chromaticities` = ACES AP0 (D60) | make_fixtures.py |
| `luma_chroma_piz.exr` | 36x22 RGB (1.25v, v, 0.75v) with v = 0.5 + x/8 + y/16, stored as Y + RY/BY subsampled 2x2, PIZ | `make_yca.cc` (libOpenEXR 3.1.5 `RgbaOutputFile`, `WRITE_YC`) |
| `mipmap_piz.exr` | pattern, mipmap, tiles 16x8, rounding down, PIZ | `exrmaketiled -m -d -t 16 8 -z piz rgb_half_zip.exr mipmap_piz.exr` (3.1.5) |
| `ripmap_rle.exr` | pattern, ripmap, tiles 8x8, rounding up, RLE | `exrmaketiled -r -u -t 8 8 -z rle rgb_half_zip.exr ripmap_rle.exr` (3.1.5) |

## Interop of Convertible's OpenEXR writer

Checked during development (not part of the unit tests) by writing files
with `convertible convert ... -t exr` from `rgb_float_zip.exr`,
`rgba_half_piz.exr`, `ya_zip.exr`, `rgb_half_dwaa.exr`, `multipart.exr` and
`image/png/rgba8.png`, for every `compression` (none, rle, zips, zip, piz)
and `pixel_type` (half, float):

- OpenEXR Python bindings 3.5.5 open every file; the stored samples equal
  the source samples exactly (float output), equal them after rounding to
  half (half output), and alpha / Y / premultiplied colour round-trip
  exactly. ZIP and PIZ output of the same PNG decode to identical samples.
- OpenImageIO `oiiotool` 2.4 and `exrheader` (OpenEXR 3.1.5, Ubuntu 24.04
  container) open every file with the expected size, channels and type;
  `oiiotool --diff` reports zero difference for float output and RGBA/YA,
  and at most the half rounding step for half output. Convertible's DWA
  decoder matches OpenEXR 3.5.5 bit for bit; OpenEXR 3.1.5 decodes the same
  DWAA file up to 0.002 differently (an upstream change between versions).
- The files hold only the eight required header attributes (`exrheader`
  also prints the `type` that the library derives when it is absent).
