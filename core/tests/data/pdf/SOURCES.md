# PDF family fixtures

All content is synthetic (lorem ipsum, made-up figures, geometric test
patterns). The documents were written for these tests; their sources are in
`src/`. Every PDF below was produced by independent software in a throwaway
Ubuntu 24.04 container by `src/make_fixtures.sh` (run with
`SOURCE_DATE_EPOCH=0`), and the reference renders and texts were packed by
`src/pack_references.py`:

```sh
docker run --rm -v "$PWD/fx:/w" <ubuntu:24.04 + the packages below> bash /w/make_fixtures.sh
docker run --rm -v "$PWD/fx:/w" <same image> python3 /w/pack_references.py
```

Tool versions: GPL Ghostscript 10.02.1, qpdf 11.9.0, poppler pdftotext 24.02.0,
pdfTeX 3.141592653-2.6-1.40.25 (TeX Live 2023/Debian), LibreOffice 24.2.7.2,
WeasyPrint 61.1, img2pdf 0.5.1, ImageMagick 6.9.12-98, libtiff 4.5.1,
jbig2enc 0.28.

## Documents

| File | Producer and command | Content the tests rely on |
| --- | --- | --- |
| `latex_article.pdf` | `pdflatex src/article.tex` (run twice) | A5, one page: sections "1 Introduction", "1.1 Details", "2 Conclusion"; bold/italic words; footnote; link to https://example.com/docs; a 2-item bullet list ($\bullet$, cmsy), a 2-item enumerate; a ruled 3x3 table (Name/Count/Price, Apples 12 3.50, Pears 7 2.25). Type 1 fonts (CM), one Type 3 bitmap font (TS1). |
| `latex_twocolumn.pdf` | `pdflatex src/twocol.tex` | two-column page; left column starts "Alpha column", right column starts "Beta column". |
| `latex_small.pdf` | `pdflatex src/small.tex` | A6 page, text "Encrypted sample text. Lorem ipsum ..." (base of the qpdf variants). |
| `gs_cff_article.pdf` | `gs -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 -o gs_cff_article.pdf article.pdf` | same text as `latex_article.pdf`, fonts converted to CFF (FontFile3/Type1C). |
| `gs_graphics.pdf` | `gs -sDEVICE=pdfwrite -o gs_graphics.pdf src/graphics.ps` | 300x300 pt: red rectangle, even-odd blue star, caps/joins strokes, dashed green line, clipped orange circle, axial shading, CMYK square. |
| `gs_type3_base14.pdf` | `gs -sDEVICE=pdfwrite -dEmbedAllFonts=false -dSubsetFonts=true -o gs_type3_base14.pdf src/text_fonts.ps` | a Type 3 font (square/triangle/bar glyphs on A/B/C) and non-embedded Helvetica, Times-Bold, Courier text: "Helvetica text: Hello World 123", "Times bold line", "mono = code;". |
| `gs_two_pages.pdf` | `gs -sDEVICE=pdfwrite -o gs_two_pages.pdf small.pdf twocol.pdf` | two pages of different sizes (A6, A5 landscape). |
| `libreoffice_report.pdf` | `soffice --headless --convert-to pdf:writer_web_pdf_Export src/report.html` | headings (h1/h2), bullet list (3 items), numbered list (2 items), bordered table Region/Q1/Q2 with North 120 135, South 98 110, East 143 151, link to https://example.org/report, accented text "café, naïve, Zürich, €", an embedded 96x64 gradient picture (`src/pattern.png`, `convert -size 96x64 gradient:red-blue -strip PNG24:pattern.png`). TrueType subsets with ToUnicode. |
| `weasyprint_report.pdf` | `weasyprint src/report.html weasyprint_report.pdf` | same text as above (no table borders: the HTML `border` attribute is ignored). |
| `qpdf_objstm.pdf` | `qpdf --deterministic-id --object-streams=generate latex_small.pdf qpdf_objstm.pdf` | object streams + cross-reference stream. |
| `qpdf_linearized.pdf` | `qpdf --deterministic-id --linearize latex_small.pdf qpdf_linearized.pdf` | linearised (hint stream, two xref sections). |
| `qpdf_uncompressed.pdf` | `qpdf --deterministic-id --stream-data=uncompress --object-streams=disable latex_small.pdf qpdf_uncompressed.pdf` | uncompressed streams, classic xref table. |
| `enc_rc4_40.pdf` | `qpdf --static-id --allow-weak-crypto --encrypt "" owner 40 -- latex_small.pdf enc_rc4_40.pdf` | RC4 40-bit (R2), empty user password. |
| `enc_rc4_128.pdf` | `... --encrypt "" owner 128 --use-aes=n ...` | RC4 128-bit (R3). |
| `enc_aes_128.pdf` | `... --encrypt "" owner 128 --use-aes=y ...` | AES-128 (V4/R4, crypt filters). |
| `enc_aes_256.pdf` | `qpdf --static-id --encrypt "" owner 256 -- ...` | AES-256 (V5/R6). |
| `enc_aes_256_user.pdf` | `qpdf --static-id --encrypt secret owner 256 -- ...` | AES-256 with user password `secret`, owner password `owner`. |
| `enc_rc4_128_user.pdf` | `qpdf --static-id --allow-weak-crypto --encrypt secret owner 128 --use-aes=n -- ...` | RC4-128 with user password `secret`. |
| `img2pdf_jpeg.pdf` | `img2pdf --producer "" --nodate photo.jpg -o img2pdf_jpeg.pdf` | the JPEG embedded unchanged (DCTDecode). |
| `img2pdf_png.pdf` | `img2pdf --producer "" --nodate shape.png -o img2pdf_png.pdf` | 64x48 navy disc on white, Flate with PNG predictor. |
| `photo.jpg` | `convert -size 64x48 gradient:yellow-green -strip -quality 90 photo.jpg` | source of `img2pdf_jpeg.pdf`. |
| `shape.png` | `convert -size 64x48 xc:white -fill navy -draw "circle 32,24 32,6" -strip PNG24:shape.png` | source of `img2pdf_png.pdf`. |
| `ccitt_g4.pdf` | `tiff2pdf -o ccitt_g4.pdf fax_g4.tif` (see bilevel below) | 200x80 CCITT G4 image (BlackIs1 true). |

## Reference renders and texts (`ref/`)

`ref/<name>.p1.gray.zz`: page 1 rendered by Ghostscript,
`gs -sDEVICE=pgmraw -r36 -dTextAlphaBits=4 -dGraphicsAlphaBits=4`, stored as a
zlib stream of `width (u32 LE), height (u32 LE), 8-bit gray pixels`.
`ref/<name>.pdftotext.txt`: `pdftotext -enc UTF-8 <name>.pdf`.

## Bi-level images (`bilevel/`)

| File | Command | Content |
| --- | --- | --- |
| `fax.pbm` | `convert -size 200x80 xc:white -fill black -draw "rectangle 10,10 60,70" -draw "circle 130,40 130,10" -draw "line 0,79 199,0" -monochrome -strip fax.png; convert fax.png fax.pbm` | the reference bitmap (P4, 1 = black). |
| `fax_g4.raw` | `tiffcp -c g4 fax_raw.tif fax_g4.tif`, strip bytes extracted by `pack_references.py` | CCITT T.6, Columns 200, Rows 80, BlackIs1 (min-is-white). |
| `fax_g3_1d.raw` | `tiffcp -c g3:1d fax_raw.tif fax_g3_1d.tif`, strip extracted | CCITT T.4 1-D (K=0), with EOLs. |
| `fax_g3_2d.raw` | `tiffcp -c g3:2d fax_raw.tif fax_g3_2d.tif`, strip extracted | CCITT T.4 2-D (K>0), with EOLs and tag bits. |
| `fax_generic.jb2` | `jbig2 -p fax.png > fax_generic.jb2` | embedded JBIG2 (page info + generic region, MQ coder). |
| `fax_tpgd.jb2` | `jbig2 -d -p fax.png > fax_tpgd.jb2` | the same with typical prediction (TPGDON). |
| `jbig2_text.pbm` | `convert -size 240x60 xc:white -fill black -pointsize 22 -draw 'text 8,40 "ABBA CAB BAD"' -monochrome -strip text.png; convert text.png jbig2_text.pbm` | reference bitmap for the symbol-coded page. |
| `text_symbols.jb2g`, `text_page.jb2` | `jbig2 -s -p -b sym text.png` (`sym.sym`, `sym.0000`) | JBIG2Globals (symbol dictionary) and page stream (text region). |

## Interoperability of the PDFs written by Convertible

Checked during development (not committed; the outputs are produced by our
own writers): Document -> PDF, Image -> PDF and PDF -> PDF (compressed)
outputs pass `qpdf --check` (no errors), list their fonts with `pdffonts`
(embedded CID TrueType subsets with ToUnicode), extract with `pdftotext` and
render correctly with Ghostscript 10.02.1 (`gs -sDEVICE=png16m`).
