# Windows metafile fixtures (WMF, EMF, EMF+, WMZ, EMZ)

All drawings are synthetic (coloured shapes, "Hello EMF", lorem ipsum). The
inputs are in `sources/` and were written by hand for these tests:

| Source | Content |
| --- | --- |
| `sources/shapes.svg` | 400 x 300 px: red rectangle (20,20,120x80) with a 4 px blue stroke; green `#00c000` ellipse centred (240,60) radii 60 x 40; yellow `#ffcc00` triangle (40,140)-(140,140)-(90,220) with a 2 px black stroke; purple `#800080` 6 px Bézier from (200,140) to (340,140); teal `#008080` 3 px polyline dashed 12/6; "Hello EMF" in DejaVu Sans 28 px navy `#000080` at baseline (20,280); a 4 x 4 PNG (red, green / blue, white quadrants) at (300,240) 40 x 40 |
| `sources/fills.svg` | 200 x 200 px: red-to-blue linear gradient rectangle (10,10,180x60); green rectangle (10,90,80x80) at fill-opacity 0.5; black square with a square hole (even-odd), outer (120,90,60x80), inner (135,105,30x50) |
| `sources/text.svg` | 300 x 120 px: "Lorem ipsum" DejaVu Sans 24 px; "Ωμέγα Жук" DejaVu Serif bold 20 px `#c00000`; "rotated" 16 px blue rotated -30 degrees about (150,110) |
| `sources/draw.fodg` | LibreOffice Draw flat document: rounded rectangle with a blue hatch on light yellow, orange pie (section 30-300 degrees, 3 cm), blue `#00a0ff` chord (lower half cut, 3 x 2 cm ellipse), green dashed arc, "Draw 123" 18 pt red |
| `sources/shapes.ps` | PostScript, 200 x 150 pt: blue 60 pt square at (20,20), red 4 pt circle stroke, green triangle, black 2 pt dashed line |

Tools (Docker image `cvt-vector-tools:local`, Ubuntu 24.04): LibreOffice
24.2.7.2, Inkscape 1.2.2, pstoedit 4.00, rsvg-convert 2.58.0; gzip 1.12 on the
host (`gzip -n` stores neither name nor time). Commands are run from this
directory; each output was renamed to the file name shown.

| File | Tool | Command | What the tests rely on |
| --- | --- | --- | --- |
| `lo_shapes.emf` | LibreOffice | `soffice --headless --convert-to emf sources/shapes.svg` | EMF+ header + GetDC, then GDI only; page = drawing bounds (372.5 x 272.5 px); red 120 x 80 rectangle; triangle 20 px right / 120 px below it; "Hello EMF" ExtTextOutW DejaVu Sans 28 px navy |
| `lo_shapes.wmf` | LibreOffice | `soffice --headless --convert-to wmf sources/shapes.svg` | placeable header (2540 units per inch), embedded-EMF escapes (skipped), TEXTOUT "Hello EMF", STRETCHDIB picture |
| `lo_text.emf` | LibreOffice | `soffice --headless --convert-to emf sources/text.svg` | three ExtTextOutW runs: "Lorem ipsum" 24 px, the Greek/Cyrillic run bold DejaVu Serif `#c00000`, "rotated" with escapement 300 |
| `lo_text.wmf` | LibreOffice | `soffice --headless --convert-to wmf sources/text.svg` | "Lorem ipsum" as TEXTOUT, non-Latin text as polygons, "rotated" with escapement |
| `lo_fills_dual.emf` | LibreOffice | `soffice --headless --convert-to emf sources/fills.svg` | dual file: the 50 % green square only as an EMF+ FillPolygon (ARGB 0x7F00FF00, 16-bit points), the GDI fallback draws it opaque between the EMF+ record and the next GetDC; gradient as many GDI polygons |
| `lo_draw.emf` | LibreOffice | `soffice --headless --convert-to emf sources/draw.fodg` | A4 page; hatch background `#ffff99`, orange pie 3 cm high, blue chord 3 x 1 cm, green dashed arc as separate dashes, "Draw 123" |
| `ink_shapes.emf` | Inkscape | `inkscape --export-type=emf --export-filename=ink_shapes.emf sources/shapes.svg` | page 400.6 x 302.3 px (frame); red rectangle exactly (20,20)-(140,100) with a 4 px blue ExtCreatePen; green ellipse (180,20)-(300,100); 6 px purple PolyBezierTo; dashed teal pen; text as 8 navy glyph paths; StretchDIBits 4 x 4 picture at (300,240) 40 px |
| `ink_shapes.wmf` | Inkscape | `inkscape --export-type=wmf --export-filename=ink_shapes.wmf sources/shapes.svg` | placeable 4999 x 3749 units at 1200 per inch (399.92 x 299.92 px); same geometry as above, curves flattened |
| `ink_fills.emf` | Inkscape | `inkscape --export-type=emf --export-filename=ink_fills.emf sources/fills.svg` | gradient replaced by its average `#7f007f`; even-odd square with a hole (2 subpaths) |
| `pstoedit_shapes.emf` | pstoedit | `pstoedit -f "emf:-OO" sources/shapes.ps pstoedit_shapes.emf` | ANISOTROPIC window/viewport, 1024 x 768 px / 320 x 240 mm reference device; blue square 60/170 of the drawing width; triangle above the square (PostScript y up) |
| `ink_shapes.emz` | gzip | `gzip -n -9 -c ink_shapes.emf > ink_shapes.emz` | same drawing as `ink_shapes.emf` |
| `ink_shapes.wmz` | gzip | `gzip -n -9 -c ink_shapes.wmf > ink_shapes.wmz` | same drawing as `ink_shapes.wmf` |
| `ink_shapes_ref.png` | rsvg-convert | `rsvg-convert -b white -w 400 -h 300 -o ink_shapes_ref.png sources/shapes.svg` | render reference for `ink_shapes.emf` (compared with PSNR/SSIM once the rasteriser is available) |

None of the files carries a description, author or host name; every file was
checked with `LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local"` (no match).

EMF+ records beyond LibreOffice's FillPolygon are not produced by any tool
available here (GDI+ is Windows-only); the EMF+ tests build their records by
hand from [MS-EMFPLUS] in `core/tests/families/vector/metafile_emfplus_test.cpp`,
as the mapping-mode, bitmap and hostile-input tests do for WMF and EMF.

## Interop checks of our writers

Files written by our EMF / WMF writers (a test scene with a stroked
rectangle, a 50 % transparent ellipse, linear and radial gradients, a dashed
Bézier, a circle-clipped square, rotated bold text, an opaque 2 x 2 picture
and a rotated 4 x 4 picture with transparent pixels; text as outlines and as
fonts) were opened with:

- LibreOffice 24.2.7.2 (`soffice --headless --convert-to png out.emf`, also
  `.wmf`, `.emz`, `.wmz`): every element renders as intended in EMF (AlphaBlend
  transparency, clip paths, gradient bands without seams, user dashes, rotated
  picture, ExtTextOutW text in fonts mode). In WMF the clip is the circle's
  bounding square (WMF clips to rectangles only), gradients are bands cut to
  the shape, transparency is blended onto white, and the transparent picture
  uses the AND-mask / OR-colour pair correctly.
- Inkscape 1.2.2 (`inkscape --export-type=png out.emf`, also `.wmf`): opens
  both. Inkscape ignores EMR_ALPHABLEND, so the translucent ellipse and the
  pictures with transparency are missing in EMF; everything else matches. In
  WMF it shows the masked picture on a black box (no SRCAND/SRCPAINT support).
- libwmf 0.2.13 (`wmf2gd -t png out.wmf`, `wmf2svg out.wmf`): renders every
  WMF element; like Inkscape it draws the masked picture on a black box.
