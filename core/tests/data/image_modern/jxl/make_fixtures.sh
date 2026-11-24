#!/bin/sh
# Encodes the JPEG XL fixtures with the reference libjxl tools. Run inside a
# throwaway Debian trixie container from this directory (after
# `python3 make_sources.py` on the host):
#   docker run --rm --memory 2g -v "$PWD":/w --workdir /w debian:trixie sh make_fixtures.sh
set -eu
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends libjxl-tools libjpeg-turbo-progs >/dev/null
cjxl --version | head -n 1

# Lossless (modular): exact against the source PNGs.
cjxl src_rgb.png lossless_rgb.jxl -d 0 -e 7 --quiet
cjxl src_rgba.png lossless_rgba.jxl -d 0 -e 7 --quiet
cjxl src_grey.png lossless_grey.jxl -d 0 -e 7 --quiet
cjxl src_rgb16.png lossless_rgb16.jxl -d 0 -e 7 --quiet
cjxl src_float.pfm lossless_float.jxl -d 0 -e 7 --quiet
cjxl src_anim.png anim.jxl -d 0 -e 7 --quiet
cjxl src_icc.png icc_lossless.jxl -d 0 -e 7 --quiet
cjxl src_orient6.png orient6.jxl -d 0 -e 7 --quiet
# Metadata in the container: Exif + XMP boxes (Brotli-compressed "brob" boxes).
cjxl src_exif.png exif_boxes.jxl -d 0 -e 7 --compress_boxes=1 --quiet
cjxl src_exif.png exif_plain.jxl -d 0 -e 7 --compress_boxes=0 --quiet

# Lossy: VarDCT at several distances, modular lossy, lossy with alpha.
cjxl src_rgb.png lossy_d1.jxl -d 1 -e 7 --quiet
cjxl src_rgb.png lossy_d3.jxl -d 3 -e 7 --quiet
cjxl src_rgb.png lossy_modular_d2.jxl -d 2 -m 1 -e 7 --quiet
cjxl src_rgba.png lossy_rgba_d1.jxl -d 1 -e 7 --quiet
# Lossless JPEG recompression (jbrd box).
cjpeg -quality 85 -outfile src_jpeg.jpg src_rgb.ppm
cjxl src_jpeg.jpg jpeg_recompressed.jxl --lossless_jpeg=1 --quiet

# Reference decodes of the lossy files by djxl (8-bit sRGB PNG).
for f in lossy_d1 lossy_d3 lossy_modular_d2 lossy_rgba_d1 jpeg_recompressed; do
  djxl "$f.jxl" "$f.ref.png" --quiet
done
djxl jpeg_recompressed.jxl jpeg_recompressed.ref.jpg --quiet
cmp src_jpeg.jpg jpeg_recompressed.ref.jpg && rm jpeg_recompressed.ref.jpg
rm -f src_rgb.ppm
for f in *.jxl; do echo "== $f"; jxlinfo "$f"; done
