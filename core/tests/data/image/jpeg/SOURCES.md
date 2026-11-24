# JPEG fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36 with
libjpeg-turbo 3.2.1, macOS 27.2 `sips`); ImageMagick runs with `-strip`.
The source picture `source.ppm` is `cvt::test::gradient_frame(45, 29)`
(deliberately not a multiple of the MCU size). The `.ppm` references are
libjpeg-turbo's own decodes (`magick X.jpg -colorspace sRGB -strip X.ppm`),
used to compare our decoder with libjpeg pixel by pixel.

| File | Content | Command |
| --- | --- | --- |
| `source.ppm` | the gradient, lossless | Pillow |
| `q90_444.jpg` (+ `.ppm`) | baseline, q90, 4:4:4 | `magick source.ppm -quality 90 -sampling-factor 1x1` |
| `q75_420.jpg` (+ `.ppm`) | baseline, q75, 4:2:0 | `-quality 75 -sampling-factor 2x2` |
| `q85_422.jpg` (+ `.ppm`) | baseline, q85, 4:2:2 | `-sampling-factor 2x1` |
| `q80_411.jpg` (+ `.ppm`) | baseline, 4:1:1 | `-sampling-factor 4x1` |
| `q80_440.jpg` | baseline, 4:4:0 | `-sampling-factor 1x2` |
| `grey.jpg` | one component | `-colorspace Gray` |
| `progressive.jpg` (+ `.ppm`) | progressive 4:2:0 | `-interlace JPEG` |
| `restart.jpg` | restart marker after every MCU | `-define jpeg:restart-interval=1` |
| `cmyk.jpg` (+ `.ppm`) | Adobe CMYK (inverted), APP14 | `-colorspace CMYK` |
| `arith.jpg` | arithmetic coding (SOF9) | `-define jpeg:arithmetic-coding=true` |
| `arith_progressive.jpg` | progressive arithmetic (SOF10) | `-interlace JPEG -define jpeg:arithmetic-coding=true` |
| `orientation6.jpg` | 16x8, red 4x4 top-left corner on blue, EXIF orientation 6 | `magick` JPEG + minimal EXIF APP1 inserted by the script |
| `pillow_icc.jpg` | gradient, optimised Huffman tables, 128-byte ICC placeholder, comment "synthetic test pattern" | Pillow `save(optimize=True, icc_profile=..., comment=...)` |
| `sips.jpg` | gradient, Apple ImageIO quality 80 | `sips -s format jpeg -s formatOptions 80` |
| `stereo.mpo` | MPO: `q90_444.jpg` with an APP2 "MPF" index, followed by `grey.jpg` | assembled by the script (CIPA DC-007 layout) |

12-bit JPEG files could not be produced with the tools on this machine
(ImageMagick's libjpeg-turbo build writes 8-bit only); the 12-bit decoding
path is untested against real files.
