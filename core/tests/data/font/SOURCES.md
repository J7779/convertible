# Font family fixtures

## Licence

Every font in this directory except `tiny_variable.*` is derived from
**Inter 4.001** (`Inter-Regular.ttf`) or **JetBrains Mono NL 2.304**
(`JetBrainsMonoNL-Regular.ttf`), both taken from `assets/fonts/` of this
repository. Both are licensed under the **SIL Open Font License 1.1**; the
licence texts are included as `OFL-Inter.txt` and `OFL-JetBrainsMono.txt`
(verbatim copies of `assets/fonts/Inter-LICENSE.txt` and
`assets/fonts/JetBrainsMono-LICENSE.txt`). The derived files keep the original
copyright and licence name records. `tiny_variable.*` is a synthetic font
drawn from scratch by the script below (no rights reserved, CC0).

## How the files were made

All files were produced in a throwaway `ubuntu:24.04` container (linux/arm64)
built from `generate/Dockerfile` (image tag `cvt-fonttools:1`) with only the
work directory mounted. Tool versions:

| Tool | Version |
| --- | --- |
| fontTools (pip, `fonttools[woff]`, brotli) | 4.68.0 |
| OpenType Sanitizer (pip `opentype-sanitizer`, `ots-sanitize`) | 9.2.0 |
| Pillow (pip) with its bundled FreeType | Pillow 13.0.0, FreeType 2.14.3 |
| FontForge (Ubuntu `fontforge-nox`, `python3-fontforge`) | 1:20230101~dfsg-1.1build1 |
| Google woff2 (`woff2_compress`) | Ubuntu `woff2` 1.0.2-2build1 |
| eot-utils (`mkeot`) | Ubuntu `eot-utils` 1.1-1build1 |
| bdftopcf (Ubuntu `xfonts-utils` 1:7.7+6build3) | 1.1 |
| bdf2psf (Ubuntu `bdf2psf`) | 1.226ubuntu1.1 |
| t1utils (`t1disasm`, check only) | 1.41-4build3 |

The scripts in `generate/` are the exact scripts that wrote every file. Work
directory layout: `src/` (the two source fonts and licences, copied from
`assets/fonts/`), `out/` (fontTools results), `ff/` (FontForge results),
`final/` (the files of this directory). Run from the work directory:

```sh
docker build -t cvt-fonttools:1 generate        # once
cp generate/*.sh generate/*.py generate/*.pe .
docker run --rm --memory 2g --network none -v "$PWD":/w -w /w cvt-fonttools:1 sh step1.sh
docker run --rm --memory 2g --network none -v "$PWD":/w -w /w cvt-fonttools:1 python3 gen_ft.py
docker run --rm --memory 2g --network none -v "$PWD":/w -w /w cvt-fonttools:1 sh step2.sh
docker run --rm --memory 2g --network none -e SOURCE_DATE_EPOCH=1704067200 -e USER=fixture -e LOGNAME=fixture \
  -v "$PWD":/w -w /w cvt-fonttools:1 fontforge -quiet -lang=py -script ff.py
docker run --rm --memory 2g --network none -e SOURCE_DATE_EPOCH=1704067200 -e USER=fixture -e LOGNAME=fixture \
  -v "$PWD":/w -w /w cvt-fonttools:1 fontforge -quiet -script ff_bitmap.pe tmp/mini_src.sfd ff/mini16
docker run --rm --memory 2g --network none -e SOURCE_DATE_EPOCH=1704067200 -e USER=fixture -e LOGNAME=fixture \
  -v "$PWD":/w -w /w cvt-fonttools:1 fontforge -quiet -script ff_bitmap.pe tmp/mono_src.sfd ff/mono16
docker run --rm --memory 2g --network none -v "$PWD":/w -w /w cvt-fonttools:1 python3 gen_ref.py
docker run --rm --memory 2g --network none -v "$PWD":/w -w /w cvt-fonttools:1 sh finalize.sh
```

The pipeline is deterministic: two clean runs produced byte-identical files.

Metadata: FontForge was run with `SOURCE_DATE_EPOCH=1704067200`
(2024-01-01T00:00:00Z) and `USER=fixture`, so the only creator/date strings it
writes are `%%Creator: fixture` and `%%CreationDate: Mon Jan  1 00:00:00 2024`
(PFA/PFB), `Comment Creation Date: Mon Jan  1 00:00:00 2024` (AFM) and
`Created by FontForge 20230101 at Sat Nov 16 00:14:01 2024 By fixture` (SVG
`<metadata>`; that date is Inter's own `head.created`). No FontForge `FFTM`
table remains in any file (`no-FFTM-table` for the dfont; for `.otb`, see
below). All `head` timestamps are the ones of the original fonts (pyftsubset
`--no-recalc-timestamp`, fontTools `recalcTimestamp=False`). Every file was
scanned (case-insensitive `grep -a`, and `strings -a`) for the login name,
the macOS and Linux home-directory prefixes, mDNS host names, the root home
directory and the container mount point, and contains no user, host or path
data.

## The subsets (base of everything else)

pyftsubset options common to both (`step1.sh`):

```
--layout-features=kern,liga,calt --hinting --name-IDs=0,1,2,3,4,5,6,13,14 --name-languages=*
--notdef-outline --no-recalc-timestamp --drop-tables+=FFTM,DSIG
```

Name records kept (platform 3 / encoding 1 / language 0x409 only; pyftsubset
drops the Macintosh records): IDs 0 copyright, 1 family, 2 subfamily, 3 unique
ID, 4 full name, 5 version, 6 PostScript name, 13 licence description,
14 licence URL. Inter: `Inter`, `Regular`, `4.001;git-9221beed3;RSMS;Inter-Regular`,
`Inter Regular`, `Version 4.001;git-9221beed3`, `Inter-Regular`. JetBrains
Mono NL: `JetBrains Mono NL`, `Regular`, `2.304;JB;JetBrainsMonoNL-Regular`,
`JetBrains Mono NL Regular`, `Version 2.304; ttfautohint (v1.8.4.7-5d5b)`,
`JetBrainsMonoNL-Regular`. FontForge outputs (`.dfont`, `.otb`) carry the same
strings in both Macintosh (platform 1) and Windows (platform 3) records.

### inter_subset.ttf (33 908 bytes)

TrueType (`glyf`) subset of Inter Regular, unitsPerEm 2048, **153 glyphs**,
cmap U+0020–U+007E plus U+00C0–U+00C5 (À Á Â Ã Ä Å). Tables: GDEF GPOS GSUB
OS/2 cmap cvt fpgm gasp glyf head hhea hmtx loca maxp name post prep (hinting
kept). **40 composite glyphs**, e.g. `Agrave`, `Aacute`, `Acircumflex`,
`Atilde`, `Adieresis`, `Aring`, `i`, `j`, `quotedbl`, `comma`, `colon`, and
unencoded `glyph00082`…; 101 code points are mapped, and the other 51
glyphs besides `.notdef` are unencoded (GSUB `liga`/`calt` closure and
composite components; the source `post` table has no names for many of them,
so fontTools calls them `glyphNNNNN`).

```sh
pyftsubset src/Inter-Regular.ttf --unicodes=U+0020-007E,U+00C0-00C5 <common options> --output-file=out/inter_subset.ttf
```

### jbmono_subset.ttf (18 344 bytes)

Same from JetBrains Mono NL Regular: unitsPerEm 1000, **100 glyphs**, every
advance 600, cmap U+0020–U+007E. Tables: GDEF OS/2 cmap cvt fpgm gasp glyf
head hhea hmtx loca maxp name post prep (no GSUB/GPOS: the NL variant has no
`kern`/`liga`/`calt` data). 4 composites (`i`, `j`, `quotedbl`, `grave`).

```sh
pyftsubset src/JetBrainsMonoNL-Regular.ttf --unicodes=U+0020-007E <common options> --output-file=out/jbmono_subset.ttf
```

## fontTools conversions (`gen_ft.py`, fontTools 4.68.0)

### inter_subset.otf (28 668 bytes)

CFF-flavoured OpenType (`OTTO`, table `CFF `, CFF version 1, no
subroutines, no hints), 153 glyphs in the same order and with the same glyph
names. Each TrueType glyph is drawn through
`ReverseContourPen(T2CharStringPen(advance, glyphSet))` (quadratic segments
converted to cubics by fontTools' BasePen, contours reversed to the CFF
counter-clockwise direction), built with `FontBuilder(2048, isTTF=False).setupCFF(...)`.
head/hhea/hmtx/cmap/name/OS/2/GDEF/GPOS/GSUB are copied from
`inter_subset.ttf`; maxp 0.5, post format 3. CFF FontBBox `-26 -480 5290 2042`.

### inter_subset_cff2.otf (27 244 bytes)

`inter_subset.otf` converted with
`fontTools.cffLib.CFFToCFF2.convertCFFToCFF2(font)`; table `CFF2`, no glyph
names in the charstring table (post format 3), 153 glyphs.

### inter_subset.cff (16 273 bytes)

The raw bytes of the `CFF ` table of `inter_subset.otf`
(`TTFont("out/inter_subset.otf").reader["CFF "]`). Font name `Inter-Regular`.

### inter_subset.woff (17 704 bytes)

WOFF 1.0 of `inter_subset.ttf`: `f.flavor = "woff"; f.save(...)` (zlib).
No metadata or private block; WOFF header version 4.66 (copied by fontTools
from `head.fontRevision`).

### inter_meta.woff (17 820 bytes)

As above, plus `WOFFFlavorData` with `metaData` (zlib-compressed in the file)
= `<?xml version="1.0" encoding="UTF-8"?><metadata version="1.0"><description><text lang="en">Test font</text></description></metadata>`
and `privData` = the 12 bytes `private-test`.

### inter_subset_ft.woff2 (14 204 bytes)

`fontTools.ttLib.woff2.compress("out/inter_subset.ttf", "out/inter_subset_ft.woff2")`
(default: `glyf`/`loca` transformed, `hmtx` not). Header version 4.66.

### jbmono_hmtx.woff2 (8 464 bytes)

`compress("out/jbmono_subset.ttf", "out/jbmono_hmtx.woff2", transform_tables={"glyf", "loca", "hmtx"})`.
The `hmtx` entry really is transformed (transform version 1, 201 bytes
stored, 400 bytes original): all left side bearings equal xMin, so the
transform drops them. 100 glyphs.

### pair.ttc (52 264 bytes)

TrueType Collection 1.0 of `inter_subset.ttf` (font 0) and
`jbmono_subset.ttf` (font 1), `TTCollection.save(..., shareTables=True)`
(identical tables are stored once). 153 + 100 glyphs.

### inter_mac_true.ttf (33 908 bytes)

`inter_subset.ttf` re-saved by fontTools after `font.sfntVersion = "true"`
(Apple sfnt tag `true` instead of 0x00010000); otherwise identical content.

### tiny_variable.ttf (1 676 bytes)

Synthetic TrueType variable font built with `FontBuilder(1000, isTTF=True)`:
glyphs `.notdef`, `space` (U+0020), `A` (U+0041), `o` (U+006F, quadratic
contours), `l` (U+006C); advances 500/250/600/600/300. `fvar` axis `wght`
100–400–900 (default 400) with named instances Thin (100), Regular (400),
Black (900); `gvar` with one tuple per glyph for wght +1.0 (stroke 180 units)
and −1.0 (stroke 30 units); the default outlines use stroke 60. E.g. at
wght=900 the `A` outline is `(0,0) (250,700) (350,700) (600,0) (420,0) (300,460) (180,0)`.
Family name `Tiny Variable`, head created/modified 2024-01-01T00:00:00Z.

## Google woff2 and mkeot (`step2.sh`)

### inter_subset.woff2 (14 204 bytes), pair.woff2 (20 460 bytes), tiny_variable.woff2 (768 bytes)

`woff2_compress <file>` (Google woff2 1.0.2) on copies of
`inter_subset.ttf`, `pair.ttc` and `tiny_variable.ttf`: `glyf`/`loca`
transformed, Brotli. `pair.woff2` has flavor `ttcf` (WOFF2 collection
directory, 2 fonts). Header version 1.0.

### inter_subset.eot (34 132 bytes)

`mkeot out/inter_subset.ttf > out/inter_subset.eot` (no URL arguments, so an
empty RootString). **EOT version 0x00020002**, Flags 0 (no MTX compression,
no XOR obfuscation), FontData = the 33 908 bytes of `inter_subset.ttf`
unchanged, Charset 0, Weight 400, magic 0x504C.

## FontForge (`ff.py`, `ff_bitmap.pe`; FontForge 20230101)

### inter_subset.dfont (45 599 bytes)

`fontforge.open("out/inter_subset.ttf").generate("ff/inter_subset.dfont", flags=("no-FFTM-table", "opentype"))`.
Mac resource-fork-in-data-fork file with one `sfnt` resource (id 2112,
TrueType, 155 glyphs: FontForge adds `.null` and `nonmarkingreturn`, keeps
instructions and GSUB/GPOS; it renames unnamed glyphs, e.g. `glyph00082` →
`glyph82`, and names some from GSUB, e.g. `hyphen_greater.calt`) and a `FOND`
resource.

### inter_subset.pfb (24 691 bytes), inter_subset.pfa (46 862 bytes), inter_subset.afm (41 952 bytes)

From `inter_subset.otf`, opened with FontForge's default encoding
(UnicodeBmp): `f.generate("ff/inter_subset.pfb", flags=("afm",))` (writes the
AFM too) and `f.generate("ff/inter_subset.pfa")`. Type 1, FontName
`Inter-Regular`, FontMatrix `[0.000488281 0 0 0.000488281 0 0]` (2048 units/em),
153 glyphs. **Encoding: a custom `/Encoding 256 array`** with code = Unicode
value for the encoded glyphs (`dup 65/A put`, …, `dup 192/Agrave put`); the
AFM says `EncodingScheme ISO10646-1`, lists 153 glyphs (unencoded ones with
`C -1`), metrics scaled to 1000 units/em, 1852 kern pairs. PFB segments:
ASCII 2546 bytes, binary (eexec) 21 592 bytes, ASCII 533 bytes, EOF.
(`t1disasm inter_subset.pfb` decodes it.)

### inter_subset.svg (80 489 bytes)

`f.generate("ff/inter_subset.svg")` from `inter_subset.otf`: SVG 1.1 font,
`<font id="Inter-Regular" horiz-adv-x="2746">` with `<font-face units-per-em="2048" ascent="1638" descent="-410" …>`,
a `<missing-glyph>`, 153 `<glyph>` elements (cubic `d` paths in font units,
y up) and 535 `<hkern>` elements.

### Bitmap fonts: mini16.* (Inter) and mono16.* (JetBrains Mono NL)

Source preparation in `ff.py`: open the subset TTF, decompose all composites
(`unlinkReferences`), remove every unencoded glyph except `.notdef`, set the
PostScript weight to `Medium` (FontForge would otherwise write BDF
`WEIGHT_NAME "Book"`, which FreeType reports as Bold), save `tmp/*_src.sfd`.
Then `ff_bitmap.pe` (FontForge native script): `BitmapsAvail([16])`,
`BitmapsRegen([16])` (FreeType rasterisation of the outlines at 16 px/em,
1 bit per pixel), and `Generate()` with bitmap types `bdf`, `otb`, `fnt`,
`fon`.

- **mini16.bdf** (13 394 bytes; FontForge writes `mini16-16.bdf`, renamed):
  BDF 2.1, `FONT -FontForge-Inter-Medium-R-Normal--16-150-75-75-P-91-ISO10646-1`,
  PIXEL_SIZE 16, proportional, FONT_ASCENT 13, FONT_DESCENT 3,
  FONTBOUNDINGBOX 15 20 0 −4, 102 chars (U+0020–007E, U+00C0–00C5, `.notdef`
  with ENCODING −1).
- **mini16.pcf** (8 564 bytes): `bdftopcf -o final/mini16.pcf final/mini16.bdf`,
  uncompressed, defaults: MSB-first bytes and bits, glyph rows padded to 4
  bytes (table format 0x0e), compressed metrics (0x10e).
- **mini16.otb** (18 768 bytes): FontForge `Generate("…otb", "otb")`,
  bitmap-only sfnt with one EBLC/EBDT strike, ppem 16, bit depth 1, 104
  glyphs, empty `glyf`/`loca` (OTS rejects it with "glyf: zero-length table",
  FreeType loads it). FontForge always adds `FFTM` and `PfEd` tables to otb;
  `finalize.sh` deletes those two tables with fontTools
  (`TTFont(..., recalcTimestamp=False, recalcBBoxes=False)`, `del t[tag]`,
  `save`), the only post-processing of a FontForge file.
- **mini16.fnt** (4 216 bytes): Windows FNT **version 0x0200** (2.0), raster,
  dfPoints 12 at 96 dpi, dfPixHeight 20, dfAscent 16, proportional
  (dfPixWidth 0), dfFirstChar 32, dfLastChar 197 (cp1252 code points).
- **mini16.fon** (4 656 bytes): FontForge `fon`: NE (16-bit Windows)
  executable with one FONT resource holding the FNT above.
- **mono16.bdf** (12 164 bytes): as mini16 but from JetBrains Mono NL:
  `-FontForge-JetBrains Mono NL-Medium-R-Normal--16-150-75-75-C-100-ISO10646-1`,
  character cell 10×16 (SPACING "C", AVERAGE_WIDTH 100, every DWIDTH 10),
  FONTBOUNDINGBOX 10 17 0 −3, 96 chars (U+0020–007E + `.notdef`).
- **mono16.pcf** (8 060 bytes): `bdftopcf -o final/mono16.pcf final/mono16.bdf`.
- **mono16.otb** (4 828 bytes), **mono16.fnt** (3 788 bytes; FNT 2.0,
  dfPixWidth 10, dfPixHeight 17, chars 32–126), **mono16.fon** (4 272 bytes):
  as for mini16.
- **mono16.psf** (8 941 bytes): **PSF2** (magic 72 B5 4A 86, version 0,
  header 32, flags 1 = Unicode table present, 256 glyphs, 32 bytes/glyph,
  height 16, width 10, 2 bytes per row), made by
  `bdf2psf --fb final/mono16.bdf /usr/share/bdf2psf/standard.equivalents /usr/share/bdf2psf/ascii.set 256 final/mono16.psf`.
  95 glyph slots are mapped (ASCII U+0020–U+007E, at their ASCII positions);
  bdf2psf fills the **161 unused slots with pseudo-random bytes**
  (`srand(0)`, deterministic) and gives them empty Unicode entries, so tests
  must only look at mapped slots. Rows that do not fit FONT_ASCENT/DESCENT
  are clipped by bdf2psf.

PSF needs a fixed cell width, which is why the PSF is made from the
JetBrains Mono strike (no `mini16.psf`).

## Reference data (`gen_ref.py`, fontTools 4.68.0, Pillow 13.0.0 / FreeType 2.14.3)

### inter_subset_ref.json (11 660 bytes)

For `inter_subset.ttf`: `numGlyphs` (153), `unitsPerEm` (2048),
`glyphOrder`, `cmap` (`"codepoint": [glyphName, gid]`), `advances` and
`lsbs` per gid (hmtx), `names` (platform 3 records, nameID → string),
`bboxes` per gid (`[xMin, yMin, xMax, yMax]` from the glyf table, `null` for
empty glyphs) and `glyphs` for U+0041 A, U+004F O, U+0067 g, U+0061 a, U+00C0
Agrave: gid, advance, composite flag and components (`[name, gid, dx, dy]`),
bbox, and `contours` = lists of `[x, y, onCurve]` from
`glyph.getCoordinates(glyfTable)` (composites flattened; implied on-curve
points are not inserted). `Agrave` (gid 4, 3 contours) is a composite of
`A` (gid 1, offset 0,0) and `glyph00120` (gid 120, offset 292,372); A, O, g,
a are simple glyphs with 2 contours each. `renders` describes the two PNGs below.

### inter_subset_otf_ref.json (10 108 bytes)

For `inter_subset.otf`: `numGlyphs`, `unitsPerEm`, `glyphOrder`, `cmap`,
`advances`, and for the same five characters the outline as recorded by
`fontTools.pens.recordingPen.RecordingPen` on the CFF glyph set:
`[op, [[x, y], ...]]` with ops `moveTo`, `lineTo`, `curveTo`, `closePath`.

### render_inter_A_64.png (96×96) and render_inter_text_32.png (400×48)

8-bit grayscale (mode `L`) FreeType renders of `inter_subset.ttf`, black
background (0), glyph coverage drawn with fill 255 (anti-aliased, FreeType
default hinting = TrueType bytecode interpreter, no kerning: the BASIC
layout engine ignores GPOS and Inter has no `kern` table). Exact code:

```python
font = ImageFont.truetype("out/inter_subset.ttf", SIZE, layout_engine=ImageFont.Layout.BASIC)
im = Image.new("L", CANVAS, 0)
ImageDraw.Draw(im).text(ORIGIN, TEXT, font=font, fill=255, anchor="ls")
im.save(PATH, optimize=False)
```

| File | TEXT | SIZE (px/em) | CANVAS | ORIGIN = (pen x, baseline y) | ink bbox (Pillow `textbbox`) |
| --- | --- | --- | --- | --- | --- |
| render_inter_A_64.png | `A` | 64 | 96×96 | (24, 72) | (24, 25)–(68, 72) |
| render_inter_text_32.png | `Hamburgefonstiv 0123` | 32 | 400×48 | (8, 36) | (8, 11)–(353, 43) |

`anchor="ls"` puts the left edge of the first pen position at x and the
baseline at y. Pen x positions (integer, hinted advances) of the 32 px text:
H 8, a 32, m 50, b 78, u 98, r 117, g 129, e 149, f 168, o 180, n 199, s 218,
t 235, i 245, v 253, space 271, 0 280, 1 300, 2 313, 3 333; total advance 345.
The A at 64 px has advance 44. These numbers are also in
`inter_subset_ref.json` → `renders`.

## Checks

`ots-sanitize` (OTS 9.2.0) accepts every `.ttf`, `.otf`, `.woff`, `.woff2`
and `.ttc` here; it rejects `.otb` (empty glyf, see above). FreeType (through
Pillow) opens every font file here except `.eot`, `.svg`, `.afm`, `.cff` and
`.psf`, which it does not support.

## generate/

`Dockerfile`, `step1.sh`, `gen_ft.py`, `step2.sh`, `ff.py`, `ff_bitmap.pe`,
`gen_ref.py`, `finalize.sh`: the scripts used above (not fixtures).
`opentype-sanitizer` has no arm64 wheel and its source build needs
`setuptools<72`, hence the pin in the Dockerfile.

## Interop checks of Convertible's own output

Not fixtures: how the font writers were verified while developing, with the
same image built from `generate/Dockerfile`. Outputs were produced from the
fixtures above with the `convertible` CLI (`ttf otf woff woff2 ttc eot dfont
cff pfa pfb afm bdf pcf psf svg` targets, subsets with `text=` / `unicodes=`,
`drop_hinting`, `preset=small`, variable and CFF2 sources).

- `ots-sanitize` (OTS 9.2.0) accepts every `.ttf`, `.otf`, `.woff`, `.woff2`
  and `.ttc` we wrote, including a WOFF2 collection.
- Google `woff2_decompress` (1.0.2) decodes every WOFF2 we wrote; for single
  fonts fontTools then finds every table equal to ours except `glyf`/`loca`
  (rebuilt by the transform), and identical glyph outlines.
- fontTools 4.68.0 opens every sfnt, WOFF and WOFF2 we wrote and draws all
  glyphs (it has no reader for WOFF2 collections).
- FreeType 2.14.3 (through Pillow) opens our `.pfa`, `.pfb`, `.cff`,
  `.dfont`, `.bdf`, `.pcf`, `.ttf`, `.otf` and `.woff` output.
- `t1disasm` (t1utils 1.41) disassembles our PFB (153 charstrings) and
  `eotinfo` (eot-utils 1.1) reads our EOT header.
