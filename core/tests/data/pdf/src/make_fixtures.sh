#!/bin/bash
# Runs inside a throwaway Ubuntu 24.04 container with the tools listed in SOURCES.md; /w is the mounted work directory.
set -e
export SOURCE_DATE_EPOCH=0 FORCE_SOURCE_DATE=1 HOME=/tmp/home
rm -rf /w/out /w/work
mkdir -p /tmp/home /w/out /w/work /w/out/ref
cd /w/work
cp /w/src/* .
O=/w/out

# ---- LaTeX (Type 1 fonts)
for t in article twocol small; do
  pdflatex -interaction=batchmode $t.tex >/dev/null
  pdflatex -interaction=batchmode $t.tex >/dev/null
done
cp article.pdf $O/latex_article.pdf
cp twocol.pdf $O/latex_twocolumn.pdf
cp small.pdf $O/latex_small.pdf

# ---- Ghostscript rewrite (Type 1 -> CFF)
gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 -o $O/gs_cff_article.pdf article.pdf

# ---- PostScript graphics and fonts through Ghostscript
gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite -o $O/gs_graphics.pdf graphics.ps
gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite -dEmbedAllFonts=false -dSubsetFonts=true -o $O/gs_type3_base14.pdf text_fonts.ps

# ---- LibreOffice (TrueType subsets, ToUnicode, tables, links, image)
convert -size 96x64 gradient:red-blue -strip PNG24:pattern.png
soffice --headless --convert-to pdf:writer_web_pdf_Export --outdir lo report.html >/dev/null 2>&1
cp lo/report.pdf $O/libreoffice_report.pdf

# ---- WeasyPrint (OpenType subsets)
weasyprint report.html $O/weasyprint_report.pdf >/dev/null 2>&1

# ---- qpdf structural and encryption variants of the small LaTeX page
B=$O/latex_small.pdf
qpdf --deterministic-id --object-streams=generate $B $O/qpdf_objstm.pdf
qpdf --deterministic-id --linearize $B $O/qpdf_linearized.pdf
qpdf --deterministic-id --stream-data=uncompress --object-streams=disable $B $O/qpdf_uncompressed.pdf
qpdf --static-id --allow-weak-crypto --encrypt "" owner 40 -- $B $O/enc_rc4_40.pdf
qpdf --static-id --allow-weak-crypto --encrypt "" owner 128 --use-aes=n -- $B $O/enc_rc4_128.pdf
qpdf --static-id --allow-weak-crypto --encrypt "" owner 128 --use-aes=y -- $B $O/enc_aes_128.pdf
qpdf --static-id --encrypt "" owner 256 -- $B $O/enc_aes_256.pdf
qpdf --static-id --encrypt secret owner 256 -- $B $O/enc_aes_256_user.pdf
qpdf --static-id --allow-weak-crypto --encrypt secret owner 128 --use-aes=n -- $B $O/enc_rc4_128_user.pdf
# Incremental update appended by qpdf? (qpdf rewrites); use pdftk-free approach: ghostscript of two files
gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=pdfwrite -o $O/gs_two_pages.pdf small.pdf twocol.pdf

# ---- img2pdf (JPEG passthrough, PNG -> Flate)
convert -size 64x48 gradient:yellow-green -strip -quality 90 photo.jpg
convert -size 64x48 xc:white -fill navy -draw "circle 32,24 32,6" -strip PNG24:shape.png
img2pdf --producer "" --nodate photo.jpg -o $O/img2pdf_jpeg.pdf
img2pdf --producer "" --nodate shape.png -o $O/img2pdf_png.pdf
cp photo.jpg shape.png $O/

# ---- CCITT via libtiff
convert -size 200x80 xc:white -fill black -draw "rectangle 10,10 60,70" -draw "circle 130,40 130,10" -draw "line 0,79 199,0" -monochrome -strip fax.png
convert fax.png -strip -compress None fax_raw.tif
tiffcp -c g4 fax_raw.tif fax_g4.tif
tiffcp -c g3:2d fax_raw.tif fax_g3_2d.tif
tiffcp -c g3:1d fax_raw.tif fax_g3_1d.tif
tiff2pdf -o $O/ccitt_g4.pdf fax_g4.tif
tiff2pdf -o $O/ccitt_g3_2d.pdf fax_g3_2d.tif
tiff2pdf -o $O/ccitt_g3_1d.pdf fax_g3_1d.tif
convert fax.png -strip $O/fax.pbm

# ---- JBIG2 (generic region, TPGD, symbol/text region)
convert -size 240x60 xc:white -fill black -pointsize 22 -draw 'text 8,40 "ABBA CAB BAD"' -monochrome -strip text.png
convert text.png -strip $O/jbig2_text.pbm
jbig2 -p fax.png > $O/fax_generic.jb2
jbig2 -d -p fax.png > $O/fax_tpgd.jb2
jbig2 -s -p -b sym text.png >/dev/null 2>&1
cp sym.sym $O/text_symbols.jb2g
cp sym.0000 $O/text_page.jb2

# ---- reference renders (36 dpi gray) and texts
for f in $O/*.pdf; do
  n=$(basename $f .pdf)
  case $n in enc_*_user) pw="-sPDFPassword=secret"; upw="-upw secret";; *) pw=""; upw="";; esac
  gs -q -dNOPAUSE -dBATCH -dSAFER $pw -sDEVICE=pgmraw -r36 -dTextAlphaBits=4 -dGraphicsAlphaBits=4 -o /w/work/ref_${n}_%d.pgm $f 2>/dev/null || true
  pdftotext $upw -enc UTF-8 $f /w/work/ref_$n.txt 2>/dev/null || true
done
{
gs --version
qpdf --version | head -1
pdftotext -v 2>&1 | head -1
pdflatex --version | head -1
soffice --version 2>&1 | head -1
weasyprint --version 2>&1
img2pdf --version 2>&1
convert -version | head -1
tiffcp 2>&1 | head -1
jbig2 -V 2>&1 | head -1
} > /w/work/versions.txt || true
ls -la $O
