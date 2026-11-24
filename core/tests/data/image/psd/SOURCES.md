# PSD/PSB and DDS fixtures

Produced by `make_fixtures.py` (Pillow 13.0.0, ImageMagick 7.1.2-36);
ImageMagick runs with `-strip`. "Gradient" = `cvt::test::gradient_frame(32, 24)`,
"alpha gradient" = `cvt::test::alpha_gradient_frame(32, 24)`.

| File | Content | Command |
| --- | --- | --- |
| `rgb8.psd` | gradient (layer + RLE composite) | `magick grad.png PSD:` |
| `grey8.psd` | gradient in grey | `-colorspace Gray PSD:` |
| `rgb16.psd` | gradient, 16 bits per channel | `-depth 16 PSD:` |
| `cmyk8.psd` | gradient in CMYK | `-colorspace CMYK PSD:` |
| `rgb8_raw.psb` | gradient, large-document format, uncompressed | `-compress None PSB:` |
| `rgba8.psd` | alpha gradient, flat 4-channel composite blended onto white | written by the script from the Photoshop file format specification |
| `rgba.dds` | alpha gradient, uncompressed BGRA | `magick alpha.png -define dds:compression=none -define dds:mipmaps=0 DDS:` |
| `dxt1.dds` | gradient, DXT1 | `-define dds:compression=dxt1` |
| `dxt3.dds`, `dxt5.dds` | alpha gradient, DXT3 / DXT5 | `-define dds:compression=dxt3|dxt5` |
| `dx10_rgba.dds` | alpha gradient, DX10 header, DXGI_FORMAT_R8G8B8A8_UNORM | written by the script from the DDS specification |

ImageMagick's RGBA PSD stores an unblended composite but reads composites as
blended onto white (as Photoshop writes them), so `rgba8.psd` is written by
hand.
