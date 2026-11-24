# Fixtures of the smaller raster formats

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36);
ImageMagick runs with `-strip`. Patterns (32x24): "gradient" =
`cvt::test::gradient_frame`, "alpha gradient" = `cvt::test::alpha_gradient_frame`,
"palette" = 200 colours, entry i = (7i, 13i, 29i) mod 256 at `(5x + 3y) % 200`,
"checker" = 4-pixel black/white checkerboard (black top-left), "wide" = 16-bit
`(x * 2039 + y * 977 + c * 12345) % 65536`.

| File | Content | Command |
| --- | --- | --- |
| `grad.ppm`, `grad_ascii.ppm` | gradient, P6 / P3 | `magick grad.png PPM:` (`-compress none` for ASCII) |
| `grey16.pgm` | wide pattern as 16-bit grey | `magick wide.ppm -colorspace Gray PGM:` |
| `checker.pbm`, `checker_ascii.pbm` | checker, P4 / P1 | `magick checker.png PBM:` |
| `alpha.pam` | alpha gradient, RGB_ALPHA | `magick alpha.png PAM:` |
| `wide.pfm` | wide pattern in linear light, little endian | `magick wide.ppm -colorspace RGB -endian LSB PFM:` |
| `alpha.qoi`, `grad.qoi` | QOI RGBA / RGB | `magick X.png QOI:` |
| `grad_rle.tga` | gradient, RLE true colour | `magick grad.png -compress RLE TGA:` |
| `alpha.tga` | alpha gradient, 32-bit | `-compress None TGA:` |
| `palette.tga` | palette, colour-mapped | `-type Palette -compress None TGA:` |
| `grey_rle.tga` | checker, RLE greyscale | `-type Grayscale -compress RLE TGA:` |
| `grad.pcx`, `palette.pcx` | 24-bit (3 planes) / 256 colours | `magick X.png PCX:` (`-type Palette`) |
| `mono.pcx` | checker, 1 bit | Pillow `convert("1").save()` |
| `wide.hdr` | wide pattern in linear light, RLE RGBE | `magick wide.ppm -colorspace RGB HDR:` |
| `wide.ff` | wide pattern, farbfeld | `magick wide.ppm FF:` |
| `checker.wbmp`, `checker.xbm` | checker | `magick checker.png WBMP:` / `XBM:` |
| `palette.xpm` | palette, 2 characters per pixel, named and hex colours | `magick pal.png -type Palette XPM:` |
| `alpha.xpm` | alpha gradient with alpha thresholded at 50% ("None" colour) | `magick alpha.png -alpha on -channel A -threshold 50% +channel XPM:` |
| `grad_rle.sgi`, `alpha.sgi` | gradient RLE / alpha gradient verbatim | `magick X.png -compress RLE|None SGI:` |
| `wide16.sgi` | wide pattern, 16 bits per channel | `magick wide.ppm -depth 16 SGI:` |
| `grad.ras`, `palette.ras`, `mono.ras` | Sun raster 24-bit / 8-bit colour map / 1-bit | `magick X.png SUN:` |
