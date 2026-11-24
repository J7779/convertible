# Image family fixtures

Fixtures live in one sub-directory per format group, each with its own
`SOURCES.md` and a `make_fixtures.py` that regenerates every file in it
(run from that directory: `python3 make_fixtures.py`).

| Directory | Formats |
| --- | --- |
| `png/` | PNG of every colour type and bit depth, Adam7, tRNS, gAMA, metadata, APNG |
| `jpeg/` | baseline 4:4:4/4:2:2/4:2:0/4:1:1/4:4:0, grey, progressive, restart markers, CMYK (Adobe), arithmetic coding, EXIF orientation, ICC, MPO |
| `gif/` | 87a/89a, interlaced, transparency, animations with every disposal method |
| `bmp/` | Windows 3 / V5 / OS/2 headers, 1/4/8/16/24/32 bits, RLE4/RLE8, bit fields, top-down |
| `tiff/` | strips, tiles, planar, LZW/Deflate/PackBits/JPEG/CCITT G3/G4/MH, 8/16-bit and float, palette, CMYK, Lab, associated alpha, BigTIFF, big-endian, multi-page, orientation |
| `ico/` | ICO (bitmap, PNG and palette entries), CUR, ICNS (iconutil output and RLE entries) |
| `misc/` | Netpbm (PBM/PGM/PPM/PAM/PFM), QOI, TGA, PCX, Radiance HDR, farbfeld, WBMP, XBM, XPM, SGI, Sun raster |
| `psd/` | PSD/PSB composites (RGB, RGBA, grey, 16-bit, CMYK) and DDS (uncompressed, DX10, DXT1/3/5) |

Tools: Pillow 13.0.0 (Python 3.14), ImageMagick 7.1.2-36 Q16-HDRI (with
libpng 1.6.59, libjpeg-turbo 3.2.1, libtiff 4.7.2), macOS 27.2 `sips` and
`iconutil`. Every picture is a synthetic test pattern (gradients,
checkerboards, palette ramps, the 16-bit formula
`(x * 2039 + y * 977 + c * 12345) % 65536`); ImageMagick output is written
with `-strip` so no dates or tool comments are stored.

## Interoperability of our encoders

Checked by converting a 96x64 noisy gradient (and an RGBA variant) with the
`convertible` CLI and opening every result with ImageMagick 7.1.2
(`magick identify`, `magick compare -metric PSNR`), Pillow 13.0 and macOS
`sips`:

- Lossless outputs decode bit-exactly in ImageMagick (PSNR "inf"): PNG (RGB,
  RGBA, palette), BMP (24-bit, 32-bit V5 alpha, RLE8), TIFF (LZW, Deflate, with
  alpha), TGA (RLE, alpha), PCX, QOI, PPM/PAM, SGI (RLE, alpha), PSD, DDS
  (BGRA), farbfeld. Pillow reads the same files with zero difference (Sun,
  PSD, SGI, PCX, TGA checked pixel by pixel).
- Lossy outputs open everywhere with the expected quality: JPEG q90 37.8 dB,
  progressive JPEG identical pixels to baseline, JPEG q50 35.9 dB, JPEG-in-TIFF
  37.8 dB, GIF 34.1 dB, PNG "small" palette 32.6 dB, BC1 37.5 dB, BC3 39.8 dB,
  Radiance HDR 42.1 dB after ImageMagick's linear-to-sRGB conversion.
- `sips` opens PNG, JPEG (baseline and progressive), GIF, BMP, TIFF (LZW,
  Deflate, JPEG), TGA, PSD and ICO output; Pillow opens all formats it
  supports (it has no farbfeld, PAM, PFM, HDR or WBMP reader).
- Known differences of other tools, not of our files: ImageMagick swaps red
  and blue in byte-encoded (RLE) 24-bit Sun rasters, so our Sun writer stores
  the standard type unless `rle` is set; ImageMagick reads PFM as
  sRGB-encoded while PFM holds linear light (as we write it); ImageMagick
  treats the fourth channel of a flat PSD as an extra channel, not
  transparency; ImageMagick writes monochrome PCX with black = 1, contrary to
  Pillow and ZSoft's convention that we follow.
