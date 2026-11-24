# image_modern/misc fixtures

Synthetic pictures only (gradients, plasma noise, checkerboards, flat shapes).
Every container or codec stream below was written by independent software,
never by Convertible. The generator scripts in this directory are the exact
recipes; tool versions are listed per section. Commands use relative paths and
run in a scratch directory (Docker containers where noted, `--memory 2g`).

## ora/ - OpenRaster and Krita

Tools: ImageMagick 7.1.2-36 (pictures), Python 3.14 `zipfile` (packages),
script `make_containers.py` (in this directory).

| File | Content |
| --- | --- |
| `layers_nomerged.ora` | 48x32 `image/openraster` package without `mergedimage.png`: background gradient `#ff0000`->`#0000ff` (`data/background.png`), a 20x12 `#00ff00` box with alpha 128 at +10+8, opacity 0.5, a 16x16 yellow disc in a nested `<stack>` at +20+10 plus +10+4, and a hidden black layer. |
| `merged.ora` | the same stack plus a `mergedimage.png` (the 24x16 checkerboard `kra_merged.png`) that readers must prefer. |
| `merged.kra` | `application/x-krita` package with `maindoc.xml`, `mergedimage.png` (= `kra_merged.png`) and `preview.png` (= `kra_preview.png`). |
| `preview_only.kra` | the same without `mergedimage.png`. |
| `ora_expected.png` | ImageMagick's composite of the visible layers: `magick l0.png \( l1.png -channel A -evaluate multiply 0.5 +channel \) -geometry +10+8 -composite l2.png -geometry +30+14 -composite ora_expected.png` |
| `kra_merged.png`, `kra_preview.png` | `magick -size 24x16 pattern:checkerboard`, `magick -size 12x8 xc:'#336699'` |

Layer pictures: `gen1.sh` (first half). Interop of Convertible's ORA writer:
GIMP 3.0.4 (Debian trixie, `file-openraster`) opened a 300x200 alpha-gradient
ORA written by `convertible convert in.png -t ora` and its flattened export
equalled the source (ImageMagick `compare -metric AE -fuzz 1%`: 0 pixels).
GIMP 2.8.16's ORA plug-in cannot load any file (an `UnboundLocalError` in the
plug-in itself), so it was not used.

## ani/ - animated cursors and DCX

Tools: ImageMagick 7.1.2-36, Python 3.14 `struct` (`make_containers.py`).

| File | Content |
| --- | --- |
| `f0.png`, `f1.png`, `f2.png` | 32x32 frames: two coloured rectangles on transparency, and a `#ff000040`->`#0000ffff` gradient (`gen1.sh`). |
| `cursor_seq.ani` | RIFF ACON: `LIST INFO` with `INAM` "Test pattern", `anih` (3 frames, 5 steps, rate 10, flags icon+sequence), `rate` = 6 12 30 6 3 jiffies, `seq ` = 0 1 2 1 0, `LIST fram` of the three frames as `.cur` files from `magick fN.png fN.cur` (`f2` with `-type TrueColorAlpha`, 32-bit). |
| `icons.ani` | the same frames as `.ico`, no `seq `/`rate`, default rate 15 jiffies. |
| `p0.png`, `p1.png`, `p2.png` | 40x30 pages: gradient, 2-colour checkerboard, plasma (`magick ... plasma:fractal -seed 3`). |
| `pages.dcx` | `magick p0.png p1.png p2.png pages.dcx` |

Interop of Convertible's DCX writer: ImageMagick reads it back pixel-exact
(`magick compare -metric AE ours.dcx ora_expected.png` = 0).

## xcf/ - GIMP documents

References are GIMP's own flattening of each document (merge visible layers,
indexed documents converted to RGB first, precision converted to 8-bit
perceptual), exported as PNG and stripped with `magick x.png -strip x.png`.

| Files | Tool and recipe |
| --- | --- |
| `rgb8`, `u8_linear`, `u16_gamma`, `u32_linear`, `half_linear`, `float_gamma`, `double_linear`, `grey8` (`.xcf` + `.png`) | GIMP 2.10.36 (Ubuntu 24.04), `make_xcf.scm`: 64x48 document, inputs `bg.png` (`magick -size 64x48 gradient:'#ff8000-#0040ff' PNG24:bg.png`) and `fade.png` (`magick -size 32x32 gradient:'#00ff0000-#00ff00ff' PNG32:fade.png`); a 60 % white box (normal mode), a 50 % blue legacy-mode box, the fade layer with its alpha moved into a layer mask, a hidden layer, an 80 % group holding a magenta child partly off canvas. XCF v11 (8-bit) and v12 (high precision), 64-bit offsets, RLE tiles. |
| `indexed.xcf`, `indexed.png` | same script: bg + fade layer, `gimp-image-convert-indexed` to 24 colours. |
| `v28_rgb`, `v28_grey`, `v28_indexed`, `v28_group` | GIMP 2.8.16 (Ubuntu 16.04), `make_xcf28.scm`: XCF v0 (`gimp xcf file`), v0 grey, v1 indexed, v3 with a layer group; 32-bit offsets. |
| `gimp3.xcf`, `gimp3.png` | GIMP 3.0.4 (Debian trixie), `gimp3.py` (python-fu-eval): background + 50 % white layer at +10+9; v11 with GIMP 3 properties and an `icc-profile` parasite. |
| `rgb8_zlib.xcf`, `rgb8_raw.xcf`, `float_gamma_zlib.xcf` | `python3 recompress_xcf.py rgb8.xcf rgb8_zlib.xcf zlib` (and `none`; `float_gamma.xcf` -> zlib): GIMP's structure kept, tiles re-stored with Python's `zlib` / uncompressed. GIMP 3.0.4 opens all three and flattens them identically to `rgb8.png` / `float_gamma.png` (0 differing pixels). |

## jls/ - JPEG-LS

Tool: CharLS 2.4.2 (the JPEG-LS reference implementation; Debian trixie
`libcharls-dev`), program `make_jls.c` (`cc -O2 make_jls.c -lcharls -lm`).
Each `name.jls` is written by CharLS from a synthetic pattern (gradient + sine,
a flat block for run mode, a checkerboard, LCG noise); `name.pam` is CharLS's
own decoding of that file.

| File | Parameters |
| --- | --- |
| `grey8`, `grey8_near3` | 64x48, 8-bit, NEAR 0 / 3 |
| `grey5` | 40x30, 5-bit (MAXVAL 31: the low-MAXVAL threshold formula) |
| `grey12` | 40x30, 12-bit |
| `grey16_near2` | 40x30, 16-bit, NEAR 2 (LSE preset parameters) |
| `rgb8_none`, `rgb8_line`, `rgb8_sample` | 48x32 RGB, interleave none / line / sample |
| `rgb8_line_near2`, `rgb8_sample_near4` | near-lossless, line / sample interleave |
| `rgb16_sample` | 33x21 RGB 16-bit |
| `rgba8_sample` | 40x30, four components |
| `grey8_custom` | custom thresholds T1=5 T2=12 T3=40 RESET=32 (LSE) |
| `rgb8_custom_near1` | custom T1=9 T2=20 T3=50, NEAR 1, sample interleave |

Interop of Convertible's writer: 16 files (grey 8/16-bit, RGB and RGBA in
all three interleave modes, NEAR 0/3/7, 16-bit) were decoded by CharLS 2.4.2
(`dec_jls.c`, not committed: `charls_jpegls_decoder_decode_to_buffer` -> PAM);
every result was byte-identical to Convertible's own decoding.

## tex/ - GPU textures

Source pictures (ImageMagick 7.1.2-36):
`src_rgba.png` = `magick -size 64x48 gradient:'#ff3000-#1050ff' \( -size 48x64 gradient:'#ffffff'-'#202020' -rotate 90 \) -compose CopyOpacity -composite -fill '#00c000' -draw 'rectangle 40,4 58,20' -fill '#ffff00' -draw 'circle 16,34 16,42' PNG32:src_rgba.png`;
`src16.png` (`-depth 16 PNG64:`), `src_grey.png` (`-colorspace gray`),
`src_rgb_odd.png` (`-alpha off -crop 61x45+0+0`); `noise_rgb.png`
(`plasma:fractal -seed 11`), `noise_rgba.png` (+ white-to-black alpha),
`noise_a1.png` (alpha thresholded at 50 %).

| Files | Tool and recipe |
| --- | --- |
| `bc1/3/4/5/7`, `etc1`, `etc2`, `etc2a8`, `eacr`, `eacrg` `_ref.png` and their `.ktx2` | Blocks: etcpak 0.9.15 (Python) from `src_rgba.png` (`make_textures.py`; EAC R11 = the red halves of the RG11 blocks). References: texture2ddecoder 1.0.6 decoding the same blocks. Containers: KTX-Software 4.4.2 `ktx create --raw --width 64 --height 48 --format <VkFormat>_BLOCK` (`make_ktx.sh`). |
| `rgba8_srgb.ktx2`, `rgba8_zlib.ktx2`, `rgba8_zstd.ktx2`, `basis.ktx2`, `rgba16.ktx2`, `r8.ktx2`, `astc8x8.ktx2` | `ktx create` from the source PNGs (`make_ktx.sh`): R8G8B8A8_SRGB plain / `--zlib 9` / `--zstd 5` / `--encode basis-lz`, R16G16B16A16_UNORM, R8_SRGB, ASTC_8x8_UNORM_BLOCK (`astc8x8_ktx2_ref.png`: texture2ddecoder on its level data). |
| `rgba8.ktx`, `rgb8_odd.ktx`, `rgba16.ktx` | `toktx` 4.4.2 (KTX 1.1). |
| `bc7_noise.ktx`, `bc7_noise_rgb.ktx`, `etc2_noise.ktx`, `etc2a1_noise.ktx`, `bc1_noise.ktx`, `bc2_noise.ktx` + `_ref.png` | Compressonator CLI 4.6.0 (linux x86_64, Docker `--platform linux/amd64`): `compressonatorcli -fd BC7 -Quality 1.0 noise_rgba.png bc7_noise.ktx`, `-fd BC7 ... noise_rgb.png`, `-fd BC2`, `-fd ETC2_RGBA1 noise_a1.png`, `-fd ETC2_RGB`, `-fd BC1`. References: texture2ddecoder on the level data (`refs.py`). They exercise BC7 modes 0, 1, 2, 7 and the ETC2 T, H, planar and punch-through modes. |
| `astc4x4/6x6/8x5/10x8/12x12.astc` + `_ref.png` | astcenc 5.7.1 (linux arm64): `astcenc-neon -cl src_rgba.png astcNxM.astc NxM -medium`, reference `astcenc-neon -dl astcNxM.astc astcNxM_ref.png`. Includes 1-3 partitions, dual-plane and void-extent blocks. |
| `rgba8888.pvr`, `etc1.pvr`, `dxt1.pvr`, `bc7.pvr`, `etc2a8.pvr`, `astc6x6.pvr` | PVR v3 header written by `make_textures.py` (Python `struct`; no PowerVR tool was available) around the source pixels, the etcpak blocks and the astcenc 6x6 blocks. |
| `rgba8888_75`, `bgra8888_72`, `dxt1_75`, `dxt5_72` (2 frames), `rgb888_74`, `i8_75` `.vtf` + `_refN.png` | srctools 2.7.0 (Python) `VTF(64, 32, version, frames, fmt)` from the top 64x32 of `src_rgba.png` with mipmaps; references are srctools' own decoding of each file, frame by frame. |

Interop of Convertible's KTX2 writer: `ktx validate` (KTX-Software 4.4.2)
accepts both the plain and the zlib-supercompressed output, and
`ktx extract` returns pixels identical to the source PNG.
