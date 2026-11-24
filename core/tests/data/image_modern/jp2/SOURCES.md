# JPEG 2000 fixtures

All pictures are synthetic test patterns (gradients, checks, stripes, a red
block, deterministic noise). Nothing here was encoded by Convertible.

## Sources (standard library Python)

`python3 -I make_sources.py` writes:

| File | Content |
| --- | --- |
| `src_rgb.ppm` | 48x40 8-bit RGB pattern |
| `src_rgba.png` | 32x24 8-bit RGBA, alpha ramp with a fully transparent corner |
| `src_gray16.pgm` | 40x32 16-bit grey ramp with low-bit detail |
| `src_s12_32x24.raw` | 32x24 signed 12-bit samples, big-endian 16-bit words |
| `src_ycc420_48x40.raw`, `src_index.pgm` | intermediate inputs (not kept) |

## OpenJPEG 2.5.3 (`opj_compress` / `opj_decompress`, Debian trixie package libopenjp2-tools)

`docker run --rm --memory 2g -v "$PWD":/w -w /w debian:trixie sh make_opj.sh`

| File | Command |
| --- | --- |
| `rgb_lossless.jp2` | `opj_compress -i src_rgb.ppm -o rgb_lossless.jp2` (5/3, one layer) |
| `rgb_lossy97.jp2`, `rgb_lossy97.ref.ppm` | `opj_compress ... -I -r 12`; reference by `opj_decompress` |
| `tiles_layers_rpcl.j2k` | `-t 16,16 -n 3 -r 30,8,1 -p RPCL` (12 tiles, 3 layers, last lossless) |
| `cprl_layers97.jp2`, `cprl_layers97.ref.ppm` | `-I -q 28,36,44 -p CPRL -n 4 -b 16,16`; reference by `opj_decompress` |
| `gray16.j2k` | `opj_compress -i src_gray16.pgm -o gray16.j2k` (16-bit lossless) |
| `s12_signed.j2k` | `opj_compress -i src_s12_32x24.raw -o s12_signed.j2k -F 32,24,1,12,s -n 5` |
| `ycc420.j2k`, `ycc420.ref.ppm` | `-i src_ycc420_48x40.raw -F 48,40,3,8,u@1x1:2x2:2x2 -n 5`; reference by `opj_decompress` (its sYCC conversion) |
| `rgba_cdef.jp2` | `opj_compress -i src_rgba.png -o rgba_cdef.jp2 -n 5` (writes a cdef box) |
| `index.j2k` (intermediate) | `opj_compress -i src_index.pgm -o index.j2k -n 5` |

## Boxes opj_compress cannot write (`python3 -I make_wrapped.py`)

The codestreams above, unchanged, wrapped in JP2/JPX boxes built by hand after
ISO/IEC 15444-1 Annex I / 15444-2 Annex M:

| File | Boxes |
| --- | --- |
| `palette.jp2` | `index.j2k` + `pclr` (16 entries, 3 columns) + `cmap` |
| `sycc420.jp2` | `ycc420.j2k` + `colr` enumerated 18 (sYCC) |
| `icc_res.jp2` | `tiles_layers_rpcl.j2k` + `colr` method 2 (synthetic 132-byte profile) + `res `/`resc` 300 x 150 dpi |
| `rgb.jpx` | codestream of `rgb_lossless.jp2` + `ftyp` brand `jpx ` (compatible `jpx `, `jp2 `, `jpxb`) + `rreq` |

References by `opj_decompress` (`docker run ... debian:trixie sh make_refs.sh`):
`palette.ref.ppm`, `sycc420.ref.ppm`, `rgb_jpx.ref.ppm` (not kept: equal to `src_rgb.ppm`).

## macOS ImageIO (`sips`, independent encoder and decoder)

```
magick src_rgb.ppm src_rgb.png
sips -s format jp2 src_rgb.png --out sips_rgb.jp2
sips -s format jp2 src_rgba.png --out sips_rgba.jp2
sips -s format png sips_rgb.jp2 --out sips_rgb.ref.png
sips -s format png sips_rgba.jp2 --out sips_rgba.ref.png
```

sips writes lossy JP2 files with an embedded sRGB ICC profile.

## Interop of files written by Convertible (checked during development)

Files written by `convertible convert` from `src_rgb.ppm` (as PNG), `src_rgba.png`
and `src_gray16.pgm` (as 16-bit PNG), decoded by OpenJPEG 2.5.3
`opj_decompress`, Pillow 11.1 (Debian trixie `python3-pil`) and macOS `sips`,
compared with the source:

| Our file | Settings | opj_decompress | Pillow | sips |
| --- | --- | --- | --- | --- |
| jp2 | default (quality 90) | 42.46 dB | 42.46 dB | 42.43 dB |
| jp2 | lossless | exact | exact | exact |
| jp2 RGBA (cdef) | lossless | exact | exact | exact (alpha kept) |
| jp2 16-bit grey | lossless | exact (16-bit) | exact (16-bit) | opens; 8-bit output, 52 dB |
| jpx | quality 60 | 37.28 dB | 37.28 dB | 37.25 dB |
| j2k | lossless, 3 layers, 16 px tiles, CPRL | exact | exact | (no raw codestream support) |
| jp2 | quality 75, 4 layers, RPCL | 39.98 dB | 39.98 dB | 39.97 dB |

ImageMagick 7.1.2 on the development Mac has no JPEG 2000 delegate, so it
could not be used for either direction.
