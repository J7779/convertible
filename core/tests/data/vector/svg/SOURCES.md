# SVG fixtures

All content is synthetic (geometric shapes, "Lorem ipsum"). Rendering references come from
independent renderers; our own writer never produced any file here.

Tools (Docker image built from `ubuntu:24.04` with `apt install inkscape librsvg2-bin
python3-matplotlib`): Inkscape 1.2.2, rsvg-convert 2.58.0 (librsvg), matplotlib 3.6.3, GNU gzip 1.12.

## Hand-written test inputs (per the SVG 1.1 / SVG 2 specifications)

These exercise reader features; they are the inputs for the independent renderings below.

| File | Content |
| --- | --- |
| `shapes.svg` | 200x150, viewBox: red rect (10,10,60,40) `#e04040`; rounded rect rx 10 ry 6 stroked `#2060c0` width 4; green circle (170,30) r 20 fill-opacity 0.5 with black stroke 2; orange ellipse; purple line with round caps; polyline with round joins; even-odd triangle; cubic/smooth/quadratic path dashed 6 3; even-odd path with arcs inside `translate(150 110) rotate(30) scale(0.8)` at opacity 0.8 |
| `paint.svg` | linear gradients (objectBoundingBox, userSpaceOnUse with href inheritance and spreadMethod reflect, gradientTransform with repeat), radial gradient with focal point, a userSpaceOnUse pattern used as fill, gradient stroke |
| `clipmask.svg` | viewBox scaling 2:1; clipPath (user space), clipPath in objectBoundingBox units with an even-odd hole, luminance mask from a gradient, `<symbol>` used twice (one rotated), markers start/mid/end with `orient="auto-start-reverse"`, a nested `<svg>` viewport that clips, an embedded 4x4 PNG data URI drawn pixelated (the PNG was written by Python 3's zlib/struct) |
| `css.svg` | `<style>` sheet with type, class, id (`!important`), descendant and child/`:first-child` selectors, an `@media` block (ignored), style attributes and `currentColor` from the root's `color="#2a7"`. Expected fills in document order: `#888`, `#e33` (stroke 2), `#33e`, `#0a0` (opacity 0.5), `#2a7`, black at fill-opacity 0.3 |
| `filters.svg` | feGaussianBlur stdDeviation 3; drop shadow built from blur/offset/flood/composite/merge; feColorMatrix saturate 0 |
| `text.svg` | "Hello SVG" (sans-serif 20 at 10,30, fill `#123`); "Mono **bold** text" (monospace 16, anchored middle at 120, tspan bold `#c00`); "Lorem ipsum" (italic 12, anchored end at 230); "spaced    out" with letter-spacing 2 (white space collapses to "spaced out") |
| `layers.svg` (source of `layers_inkscape.svg`, not committed) | three Inkscape layers "Background", "Shapes", "Hidden" (display:none), title "Layer test", 100x50 mm |

## Files produced by independent software

| File | Tool and command |
| --- | --- |
| `shapes_rsvg.png`, `paint_rsvg.png`, `clipmask_rsvg.png`, `css_rsvg.png`, `filters_rsvg.png`, `text_rsvg.png` | `rsvg-convert -f png -o <name>_rsvg.png <name>.svg` |
| `shapes_inkscape_plain.svg` | `inkscape --export-plain-svg --export-filename=shapes_inkscape_plain.svg shapes.svg` |
| `text_inkscape_paths.svg` | `inkscape --export-plain-svg --export-text-to-path --export-filename=text_inkscape_paths.svg text.svg` (the text as DejaVu outlines) |
| `layers_inkscape.svg` | `inkscape --export-type=svg --export-filename=layers_inkscape.svg layers.svg` (Inkscape SVG: sodipodi:namedview and editor attributes, used to check that writing strips them) |
| `layers_inkscape_rsvg.png` | `rsvg-convert -f png -o layers_inkscape_rsvg.png layers_inkscape.svg` |
| `plot_matplotlib.svg` | matplotlib 3.6.3, SVG backend: `svg.fonttype = "none"`, `svg.hashsalt = "fixture"`, figure 2.4x1.6 in at 72 dpi, `ax.plot([0,1,2,3],[1,3,2,4], color="#d62728", lw=2, marker="o")`, `ax.fill_between([0,1,2,3],[0,1,1,2], color="#1f77b4", alpha=0.4)`, title "Lorem", saved with `metadata={"Date": None, "Creator": None, "Format": None, "Type": None}` |
| `plot_matplotlib_rsvg.png` | `rsvg-convert -f png -o plot_matplotlib_rsvg.png plot_matplotlib.svg` |
| `paint.svgz` | `gzip -9 -n -c paint.svg > paint.svgz` |

## Interop checks of our output (development, not part of the test run)

- Our SVG written from every fixture above (default and `minify=true`) is well-formed
  (`xmllint --noout`) and renders in rsvg-convert 2.58 and Inkscape 1.2.2 like the source:
  PSNR of their rendering of our file against our own rendering of the source was the same as for
  the source itself (shapes 47.8 dB, paint 33.3, clipmask 44.5 / Inkscape 54.6, css 57.5,
  filters 55.0 / Inkscape 45.3, layers 55.0 / 62.5, matplotlib plot 23.2 because of font
  substitution).
- A colour-traced PNG (`trace=color`, 12 colours) renders in rsvg-convert at 18.5 dB PSNR against
  the source picture (posterised gradients, as expected for 12 colours).
