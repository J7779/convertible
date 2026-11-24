# image_modern family fixtures

Fixtures live in one sub-directory per format group. Each has its own
`SOURCES.md` with the tool, version and exact command for every file, and the
results of the interoperability checks of our encoders against independent
decoders.

| Directory | Formats | Independent tools |
| --- | --- | --- |
| `webp/` | WebP lossy, lossless, alpha, animation, ICC/EXIF/XMP | libwebp 1.6.0 tools (cwebp, dwebp, img2webp, gif2webp, webpmux, anim_dump), ImageMagick 7 |
| `heif/` | AVIF and HEIC/HEIF: grids, 10-bit, alpha, EXIF orientation, sequences, PQ | macOS `sips`, libheif `heif-enc` (aom, x265), libavif `avifenc`/`avifdec` |
| `jxl/` | JPEG XL lossless/lossy, alpha, animation, 16-bit, float, ICC, Exif/XMP boxes, recompressed JPEG | libjxl `cjxl`/`djxl` 0.11.2 |
| `jp2/` | JPEG 2000 JP2/J2K/JPX: 5/3, 9/7, tiles, layers, 16-bit, signed, palette, sYCC, cdef alpha | OpenJPEG 2.5.3 `opj_compress`/`opj_decompress`, macOS `sips` |
| `exr/` | OpenEXR: every compression, half/float/uint, tiled, mipmap/ripmap, multi-part, luminance/chroma | OpenEXR 3.5.5 Python bindings, OpenEXR 3.1.5 tools, oiiotool |
| `raw/` | Synthetic DNG (uncompressed, LJ92, deflate, lossy JPEG tiles, linear) and minimal CR2/NEF/ARW/maker containers | generator script; LibRaw (rawpy) as reference decoder only |
| `misc/` | ORA, KRA, ANI, DCX, XCF, JPEG-LS, KTX/KTX2, PVR, ASTC, VTF | ImageMagick 7, GIMP 2.8/2.10/3.0, CharLS 2.4.2, etcpak, KTX-Software, Compressonator, astcenc, srctools |

All pictures are synthetic test patterns; no photographs and no device EXIF.
