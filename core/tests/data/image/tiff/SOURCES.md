# TIFF fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0 with libtiff, ImageMagick
7.1.2-36 with libtiff 4.7.2); ImageMagick runs with `-strip`. "Gradient" is
`cvt::test::gradient_frame(32, 24)`; the 16-bit pattern is
`(x * 2039 + y * 977 + c * 12345) % 65536`.

| File | Content | Command |
| --- | --- | --- |
| `rgb8_none.tif` | gradient, uncompressed | `magick grad.ppm -compress None` |
| `rgb8_lzw_predictor.tif` | LZW + horizontal predictor | `-compress LZW -define tiff:predictor=2` |
| `rgb8_packbits.tif` | PackBits | `-compress RLE` |
| `rgb8_lzw_bigendian.tif` | big-endian ("MM") | `-compress LZW -endian MSB` |
| `rgb8_tiled_deflate.tif` | 16x16 tiles, Deflate | `-compress Zip -define tiff:tile-geometry=16x16` |
| `rgb8_planar.tif` | PlanarConfiguration 2 | `-compress None -interlace Plane` |
| `rgb8_bigtiff.tif` | BigTIFF | `-compress LZW TIFF64:` |
| `jpeg.tif` | JPEG compression (new style), q90 | `-compress JPEG -quality 90` |
| `cmyk.tif` | separated (CMYK) | `-colorspace CMYK -compress LZW` |
| `lab.tif` | CIE L*a*b* (8-bit) | `-colorspace Lab -compress None` |
| `orientation6.tif` | Orientation = 6 (RightTop) | `-orient RightTop` |
| `rgba8_deflate.tif` | alpha gradient, unassociated alpha | `magick alpha.png -compress Zip` |
| `rgba8_associated.tif` | alpha gradient premultiplied, ExtraSamples = 1 | written by the script from the TIFF 6.0 specification |
| `rgb16_lzw.tif` | 16-bit pattern, LZW + predictor | `magick wide.ppm -compress LZW -define tiff:predictor=2` |
| `grey16_deflate.tif` | 16-bit grey | `-colorspace Gray -compress Zip` |
| `float32_predictor3.tif` | 32-bit float, Deflate + floating-point predictor | `-define quantum:format=floating-point -depth 32 -compress Zip -define tiff:predictor=3` |
| `float16.tif` | 16-bit float | `-define quantum:format=floating-point -depth 16 -compress None` |
| `palette8.tif` | 200-colour palette pattern | `magick pal.png -type Palette -compress LZW` |
| `mono_g4.tif` | 61x23 checkerboard of 4 px cells with an inverted diagonal, CCITT Group 4 | Pillow `compression="group4"` |
| `mono_g3.tif` | the same, Group 3 | Pillow `compression="group3"` |
| `mono_mh.tif` | the same, CCITT modified Huffman (compression 2) | Pillow `compression="tiff_ccitt"` |
| `mono_packbits.tif` | the same, PackBits | Pillow `compression="packbits"` |
| `mono_g3_lsb.tif` | the same, Group 3, FillOrder 2 | `magick bw.png -compress Fax -define tiff:fill-order=lsb` |
| `multipage.tif` | 3 pages: gradient, its negative, its mirror | `magick grad.ppm ( grad.ppm -negate ) ( grad.ppm -flop ) -compress LZW` |

ImageMagick's own associated-alpha output stores straight colour under
ExtraSamples = 1, which is why `rgba8_associated.tif` is written by hand.
