# PostScript / EPS / AI fixtures

All content is synthetic (shapes, "Lorem", "Ipsum", made-up data points).
Header comments that tools add about themselves, the user or the time
(`%%Creator:`, `%%CreationDate:`, `%%Title:`, `%%For:`, `%%Invocation:`)
were removed afterwards with
`sed -e '/^%%Creator:/d' -e '/^%%CreationDate:/d' -e '/^%%Title:/d' -e '/^%%For:/d' -e '/^%%Invocation:/d'`;
nothing else was edited. Every file was scanned for user names and home
paths (none found). Tools ran in the `cvt-vector-tools` Docker image
(Ubuntu 24.04) unless noted.

## Hand-written PostScript (inputs of the Ghostscript fixtures)

| file | content |
|---|---|
| `hand_shapes.eps` | red rectfill, blue 4 pt circle stroke, green triangle, dashed grey line, Helvetica "Lorem" (not embedded), CMYK square clipped by a circle |
| `hand_grad.eps` | axial and radial `shfill` (LanguageLevel 3) inside clip paths, 4x2 RGB `colorimage` through ASCIIHexDecode |
| `hand_multi.ps` | two pages (`%%Pages: 2`), `setpagedevice` 200x100, Times-Roman "Ipsum" |
| `hand_type1.eps` | a synthetic Type 1 font "SynthBlocks" (square, triangle, diamond; unencrypted charstrings, lenIV -1) showing "AB C" |
| `hand_jpeg.eps` | a 16x8 JPEG (ImageMagick 7 `magick -size 16x8 gradient:red-blue -quality 90 -strip grad16.jpg`) through `/ASCII85Decode filter /DCTDecode filter` in an image dictionary (ASCII85 by Python `base64.a85encode`) |

## Ghostscript 10.02.1 (`gs`)

Each is gzip-compressed (`gzip -9 -n`) because the eps2write/ps2write prolog
(a PDF interpreter written in PostScript) is about 160 KB; tests gunzip them.

| file | command |
|---|---|
| `shapes_gs.eps.gz` | `gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=eps2write -sOutputFile=shapes_gs.eps hand_shapes.eps` |
| `multi_gs.ps.gz` | `gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=ps2write -sOutputFile=multi_gs.ps hand_multi.ps` |
| `type1_gs.eps.gz` | `gs -q -dNOPAUSE -dBATCH -dSAFER -dEmbedAllFonts=true -sDEVICE=eps2write -sOutputFile=type1_gs.eps hand_type1.eps` (the font is re-embedded as an eexec-encrypted Type 1 font program) |

Render references (host Ghostscript 10.08.0, 48 dpi, anti-aliased):
`gs -q -dNOPAUSE -dBATCH -dSAFER -dEPSCrop -sDEVICE=png16m -r48 -dTextAlphaBits=4 -dGraphicsAlphaBits=4 -sOutputFile=<name>_gs48.png <name>.eps`
for `hand_shapes`, `hand_grad`, `drawing_inkscape`, `mpl_type42`.

## Inkscape 1.2.2 (cairo 1.18 PostScript surface)

Source `drawing.svg` (not stored): 200x120 px; rect with a red-blue linear
gradient, green circle with black 3 px stroke, yellow triangle with a dashed
blue stroke, purple "Lorem" in DejaVu Sans 16 px. `image.svg`: a 120x80 px
background rect, an 8x8 checkerboard PNG and a translucent circle (cairo
rasterises the drawing into one fallback image because of the opacity; the
checkerboard does not survive Inkscape's export).

| file | command |
|---|---|
| `drawing_inkscape.eps` | `inkscape --export-type=eps --export-filename=drawing_inkscape.eps drawing.svg` (Type 42 font subset, shfill, cairo prolog) |
| `drawing_inkscape.ps` | `inkscape --export-type=ps --export-filename=drawing_inkscape.ps drawing.svg` |
| `image_inkscape.eps` | `inkscape --export-type=eps --export-filename=image_inkscape.eps image.svg` (346x221 Flate + ASCII85 image) |

## matplotlib 3.6.3 (PS backend)

`fig, ax = plt.subplots(figsize=(2.5, 2.0), dpi=72)`; a line `[0,1,2,3] -> [0,1,0.5,2]`
(#1f77b4, 2 pt), two bars (#ff7f0e), ticks 0-3 / 0-2, title "Lorem";
`fig.savefig(name, metadata={"Creator": None})` with `rcParams["ps.fonttype"]` 3 and 42.

| file | font type |
|---|---|
| `mpl_type3.eps` | DejaVu Sans converted to a Type 3 font (BuildGlyph, procedures per glyph) |
| `mpl_type42.eps` | DejaVu Sans as a Type 42 font (sfnts) |

## GNU plotutils 2.6 (`graph`)

Data `0 0 / 1 2 / 2 1 / 3 3`.

| file | command |
|---|---|
| `graph_plotutils.ai` | `graph -T ai -X X -Y Y < data.txt` (Illustrator 5 format; references the Adobe procsets without including them) |
| `graph_plotutils.ps` | `graph -T ps -X X -Y Y -C < data.txt` (idraw-style PostScript with its own prolog) |

## Interop checks of our output

Our writer's files were checked with Ghostscript 10.08.0 on the host
(`gs -dSAFER -sDEVICE=png16m`), comparing renderings of the source fixture and
of our re-written file (read with our reader, written with our writer):

| source -> our output | result |
|---|---|
| `hand_shapes.eps` -> EPS | renders without errors; identical geometry (PSNR 25 dB, differences: Helvetica vs bundled Inter, CMYK conversion) |
| `shapes_gs.eps` -> EPS | renders; PSNR 24 dB (same differences) |
| `hand_grad.eps` -> EPS (level 2, `/shfill where` guarded) | renders; PSNR 46.7 dB |
| `drawing_inkscape.eps` -> EPS (level 2 and level 3 with `ps_text=text`) | renders; PSNR 26.5 dB (glyph rasterisation differences) |
| `drawing_inkscape.ps`, `multi_gs.ps`, `graph_plotutils.ps`, `image_inkscape.eps` -> PS | render without errors, all pages |
| `mpl_type3.eps` / `mpl_type42.eps` -> EPS (outlines and `ps_text=text`) | render; PSNR 34.0 / 24.8 dB |
| `graph_plotutils.ai` -> EPS | renders (the source itself cannot be rendered by Ghostscript: it lacks the Illustrator procsets) |

A throwaway PDF assembled from `pdf_content` output (shapes, translucent
fill, gradient with translucent stops, opacity group, luminosity mask,
picture with alpha) was opened with `gs -sDEVICE=pdfwrite` and
`-sDEVICE=png16m` without errors; the rendering matched the scene
(transparency, soft masks and the image SMask as intended).
