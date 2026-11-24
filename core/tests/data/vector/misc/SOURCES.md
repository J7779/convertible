# Fixtures of the smaller vector formats (VectorDrawable, Lottie, PICT)

All content is synthetic (geometric shapes, the words "Hello" / "Caf"). Commands use relative paths;
the scripts and the source SVGs are listed with each file. Every file was checked with
`LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local"` (no match).

## Android VectorDrawable

Produced by **svg2vectordrawable 2.9.1** (npm) in the `node:22-bookworm-slim` Docker image
(`npm install svg2vectordrawable@2`), from the synthetic SVGs in this directory:

| File | Command | Content the tests rely on |
| --- | --- | --- |
| `shapes.svg` | hand-written SVG source | 48x48 px, viewBox 24: red rect 2,2 8x6; circle (dropped by the tool); arc path 2..10 x 10..20 #336699; yellow 6x6 square rotated 45 deg at (17,17); even-odd square ring at 12..16 |
| `shapes_vd.xml` | `npx s2v -i shapes.svg -o shapes_vd.xml` | 4 paths, width/height 48dp, viewport 24, a group with rotation 45 and translate 17,17, fillType evenOdd |
| `strokes.svg` | hand-written SVG source | circle path with fill-opacity .5 and a 1.5 round stroke; purple chevron stroke (opacity .25, square cap, bevel join); clipped orange rect (clip dropped by the tool); teal square scaled (2, .5) at (18,20) |
| `strokes_vd.xml` | `npx s2v -i strokes.svg -o strokes_vd.xml` | #8000FF00 fill, #FF0000FF stroke 1.5 round/round; #40800080 stroke 2 square/bevel; group scaleX 2 scaleY .5 |
| `gradient.svg` | hand-written SVG source | linear red->blue 0..32 on the top half, radial white->green centre (16,24) r 8 |
| `gradient_vd.xml` | `npx s2v -i gradient.svg -o gradient_vd.xml` | two `aapt:attr` gradients (linear startX 0 endX 32; radial centre 16,24 radius 8) |
| `features_hand.xml` | **hand-written per the VectorDrawable specification** (no converter emits these features) | 200x100dp, viewport 100x50, alpha .8; group rotation 90 about pivot (25,25); clip-path in a translated group; trimPathStart .25 / End .75 on a line 0..100; sweep gradient; `@color/` reference with fillAlpha .5 |

## Lottie

Produced by **python-lottie 0.7.1** (`pip install lottie==0.7.1` in a throwaway venv in the
`cvt-vector-tools:local` image, Ubuntu 24.04, Python 3.12). Script `make_lottie.py` (in this directory;
development copy) builds the animations with `lottie.objects` and writes them with
`lottie.exporters.core.export_lottie`:

| File | Command | Content |
| --- | --- | --- |
| `lottie_shapes.json` | `python make_lottie.py` (then renamed to lottie_*.json) | 120x80, 30 fps: red rect centre (20,20) 30x20; blue ellipse (60,20) 20x20 with black stroke 2; yellow 5-point star at (95,20), radii 15/7; green bezier path with a curved edge; purple hexagon (radius 12) in a group with 50% opacity |
| `lottie_animated.json` | `python make_lottie.py` (then renamed to lottie_*.json) | 100x100, 60 frames: 20x20 red square whose layer moves (20,50)->(80,50), rotates 0->90 and fades 100->50->100 (linear keyframes) |
| `lottie_gradient_precomp.json` | `python make_lottie.py` (then renamed to lottie_*.json) | linear gradient fill red->blue from (5,15) to (45,15); dashed stroke (6 on, 4 off, width 3); precomp `comp_0` (green 20x20 square) placed at (70,30) with anchor (10,10), size 20x20 |
| `lottie_from_svg.json` | `lottie_convert.py strokes.svg lottie_from_svg.json` (input: `strokes.svg` above) | SVG imported as shape layers with `sh` paths and group transforms |
| `lottie_from_svg.lottie` | `lottie_convert.py strokes.svg lottie_from_svg.lottie` | the same as a dotLottie ZIP (`manifest.json`, `animations/animation_0.json`; the manifest names the generator, no personal data) |
| `lottie_features_hand.json` | **hand-written per the Lottie (Bodymovin 5.x) schema**: features python-lottie does not emit together | 200x100, 25 fps: null parent with a hold keyframe (0,0)->(100,0) at frame 25; parented blue box; trim paths 25..75% on a 200-wide line; solid 50x50 #ff8000 layer with an add mask (left half) and ease-in-out opacity 0->100; 2x2 green PNG image asset as a data URI (PNG from ImageMagick 7.1.2: `magick -size 2x2 xc:'#00ff00' -strip PNG32:px.png`); a layer visible only from frame 40 |

## PICT

| File | Tool / command | Content |
| --- | --- | --- |
| `gradient_truecolor.pct` | ImageMagick 7.1.2-36: `magick -size 16x8 gradient:red-blue -strip PICT:gradient_truecolor.pct` | 512-byte header, version 2 (extended header), DirectBitsRect 32-bit packType 4; rows from (255,0,0) to (0,0,255), row 3 = (146,0,109) |
| `palette.pct` | `magick -size 12x6 xc:white -fill red -draw "rectangle 0,0 5,5" -fill '#0000ff' -draw "rectangle 6,0 11,2" -type Palette -colors 4 -strip PICT:palette.pct` | PackBitsRect 8-bit with a 3-entry colour table: red left half, blue top-right 6x3, white elsewhere |
| `jpeg.pct` | `magick -size 16x16 gradient:yellow-green -strip -compress JPEG -quality 90 PICT:jpeg.pct` | QuickTime-compressed (opcode 0x8200) JPEG, 16x16, yellow top (255,255,1) to green bottom (0,128,0) |
| `vector_v2_hand.pct` | **hand-written per "Inside Macintosh: Imaging With QuickDraw" appendix A** with a Python 3 stdlib script `make_vector_pict.py` (`python3 -I make_vector_pict.py vector_v2_hand.pct vector_v1_hand.pct`); no available tool writes vector PICT (LibreOffice 24.2 has no PICT export filter, ImageMagick writes raster PICT only) | 200x100 pt, version 2: clip region, comments, red paintRect 10,10-30,50; blue frameOval 4x4 pen; green Line (10,50)->(100,50) and ShortLine with a 2x2 pen; orange paintPoly triangle; purple paintArc 90/90 in 120,10-160,50; frameRRect; fontName Helvetica (id 21), size 14, bold, LongText "Hello" at (10,95), DHText "Caf" +40 |
| `vector_v1_hand.pct` | same script | 60x40 pt, version 1 (one-byte opcodes, no file header): 50% grey PnPat paintRect, black FillPat fillOval at 30..55, ShortLine |

## Interop checks (our output read by independent tools)

- TikZ: our `.tikz` output (standalone option) of every VectorDrawable and Lottie fixture above and of
  `vector_v2_hand.pct` compiles with pdfTeX 3.141592653-2.6-1.40.25 (TeX Live 2023/Debian; packages
  texlive-latex-base, texlive-pictures, texlive-latex-extra installed in a throwaway container from
  `cvt-vector-tools:local`, `pdflatex -interaction=nonstopmode -halt-on-error`). Renderings
  (`pdftoppm -r 96`) were inspected: orientation, the 45 degree rotation (cm convention), clipping,
  pivoted rotation, sweep wedges and trims match the sources.
- VectorDrawable: our `.xml` output (from `lottie_shapes.json`, `vector_v2_hand.pct`, `gradient_vd.xml`
  with precision 2 and `strokes_vd.xml`) was converted back to SVG by the independent npm package
  **vector-drawable-svg 1.1.4** (`node:22-bookworm-slim`, `transform(xml)`) and rendered with
  **rsvg-convert 2.58.0**; the renderings match the sources (shapes, colours, alphas, strokes, the
  gradients and the group transforms). No Android tooling was available to render VectorDrawables
  directly.
