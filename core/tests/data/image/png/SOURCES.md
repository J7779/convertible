# PNG fixtures

All files are produced by `make_fixtures.py` in this directory (Pillow 13.0.0,
ImageMagick 7.1.2-36 with libpng 1.6.59, macOS 27.2 `sips`). ImageMagick runs
with `-strip -define png:exclude-chunks=date,time,tEXt,zTXt,bKGD`.
"Gradient" is `cvt::test::gradient_frame(32, 24)`; "alpha gradient" is
`cvt::test::alpha_gradient_frame(32, 24)`; the 16-bit pattern is
`(x * 2039 + y * 977 + c * 12345) % 65536` with alpha 65535 except where
`(x + y) % 3 == 0` (alpha `(x * 4096) % 65536`), 16x12 pixels.

| File | Content | Tool / command |
| --- | --- | --- |
| `rgb8.png` | gradient, colour type 2 | Pillow `Image.save` |
| `rgba8.png` | alpha gradient, colour type 6 | Pillow |
| `rgb8_interlaced.png` | gradient, Adam7 | `magick rgb8.png -interlace PNG PNG24:...` |
| `rgba8_interlaced.png` | alpha gradient, Adam7 | `magick rgba8.png -interlace PNG PNG32:...` |
| `grey1.png`, `grey2.png`, `grey4.png`, `grey8.png` | grey ramp, level `ramp(x) >> (8 - depth)` | Pillow, then `magick ... -depth N -define png:color-type=0 -define png:bit-depth=N` |
| `grey16.png`, `greyalpha16.png`, `rgb16.png`, `rgba16.png` | 16-bit pattern, colour types 0/4/2/6 | `magick tmp.pam -define png:color-type=T -define png:bit-depth=16` |
| `rgba16_interlaced.png` | 16-bit RGBA, Adam7 | `magick tmp.pam -interlace PNG ...` |
| `greyalpha8.png` | alpha gradient converted to LA | Pillow `convert("LA")` |
| `palette8.png` | 200 colours, entry i = (7i, 13i, 29i) mod 256, pixel `(5x + 3y) % 200` | Pillow mode P |
| `palette4.png`, `palette2.png`, `palette1.png` | 16/4/2 colours, entry i = (70i, 130i, 255 - 50i) mod 256, pixel `(x + y) % n` | Pillow `save(bits=N)` |
| `palette_trns.png` | 8 colours with per-entry alpha 0, 64, 128, 192, 255... | Pillow `transparency=bytes(...)` |
| `rgb_trns.png` | gradient with pixel (0,0) = key colour (1,2,3) | Pillow `transparency=(1, 2, 3)` |
| `grey_trns.png` | grey `(8x) % 256`, key 0 | Pillow `transparency=0` |
| `gamma10.png` | 16x1 linear ramp `17i`, gAMA = 1.0 | Pillow + gAMA chunk inserted by the script |
| `metadata.png` | 4x2 black with red (0,0); tEXt Title, zTXt Comment, iTXt Description "café ✓", pHYs 150 dpi, eXIf orientation 6 | Pillow `PngInfo`, `dpi=`, `exif=` |
| `anim.png` | APNG 32x24, 3 frames of growing bars (colour (80n, 255-80n, 40)), delays 100/200/300 ms, 2 plays, blend source/over/source | Pillow `save_all=True` |
| `sips_rgb8.png` | gradient | `sips -s format png` from a BMP written by `magick` |
