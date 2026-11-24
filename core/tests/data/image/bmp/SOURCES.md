# BMP fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36, macOS
27.2 `sips`). "Gradient" is `cvt::test::gradient_frame(32, 24)`, "alpha
gradient" `cvt::test::alpha_gradient_frame(32, 24)`; the 200-colour palette
pattern has entry i = (7i, 13i, 29i) mod 256 at pixel `(5x + 3y) % 200`; the
16-colour pattern entry i = (70i, 130i, 255 - 50i) mod 256 at `((x / 3) + y) % 16`.

| File | Content | Command |
| --- | --- | --- |
| `rgb24.bmp` | gradient, BITMAPINFOHEADER, 24-bit | `magick grad.png BMP3:` |
| `rgba32_v5.bmp` | alpha gradient, V5 header, BI_BITFIELDS | `magick alpha.png BMP:` |
| `pal8.bmp` | 200-colour pattern, 8-bit, uncompressed | `magick pal.png -type Palette -compress None BMP3:` |
| `pal4.bmp` | 16-colour pattern, 4-bit | `-type Palette -compress None BMP3:` |
| `rle8.bmp` | 200-colour pattern, RLE8 | `-type Palette -compress RLE BMP3:` |
| `rle4.bmp` | 8x4, RLE4 with delta, encoded, absolute and alternating runs (see the script for the exact pixels; the skipped bottom row is transparent) | written by the script from the BMP specification (ImageMagick has no RLE4 writer) |
| `rgb565.bmp` | gradient, 16-bit R5G6B5 bit fields | `-define bmp:subtype=RGB565 BMP:` |
| `argb1555.bmp` | alpha gradient, 16-bit A1R5G5B5 | `-define bmp:subtype=ARGB1555 BMP:` |
| `os2.bmp` | gradient, OS/2 1.x (12-byte) header | `magick grad.png BMP2:` |
| `sips.bmp` | gradient | `sips -s format bmp` |
| `mono1.bmp` | 4-pixel checkerboard, 1-bit | Pillow mode "1" |
| `topdown.bmp` | `rgb24.bmp` with negative height and rows reversed | the script |
