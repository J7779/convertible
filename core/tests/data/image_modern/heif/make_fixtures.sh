#!/bin/sh
# Regenerates the HEIF/AVIF decode fixtures and their independent reference
# decodes. Run from an empty scratch directory, with this directory as $1:
#   sh <dir>/make_fixtures.sh <dir>
# Needs python3, libheif's heif-enc / heif-dec (aom and x265 plug-ins),
# ImageMagick (to write one JPEG and crop one PNG), macOS sips for the
# Apple-encoded files, and Docker for the libavif tools.
set -eu
python3 -I "$1/make_sources.py" src

heif-enc -A -q 75 -o avif_420.avif src/card_64x48.png
heif-enc -A -q 85 -p chroma=444 -b 10 -o avif_444_10bit.avif src/ramp16_48x32.png
heif-enc -A -L -o avif_lossless.avif src/card_33x21.png
heif-enc -A -q 80 -o avif_alpha.avif src/alpha_48x32.png
heif-enc -A -q 80 --cut-tiles 32 -o avif_grid.avif src/card_80x48.png
heif-enc -A -q 70 -S --timebase 1000 --duration 100 -o avif_sequence.avif src/seq_1.png src/seq_2.png src/seq_3.png
heif-enc -A -q 85 -b 10 --colour_primaries 9 --transfer_characteristic 16 --matrix_coefficients 9 \
  -o avif_pq_10bit.avif src/ramp16_48x32.png
heif-enc -q 75 -o heic_420.heic src/card_64x48.png
heif-enc -q 85 -b 10 -o heic_10bit.heic src/ramp16_48x32.png
heif-enc -q 80 --cut-tiles 32 -o heic_grid.heic src/card_80x48.png
heif-enc -q 80 -o heic_alpha.heic src/alpha_48x32.png
heif-enc -q 80 --rotate-cw 90 --flip-h -o heic_irot_imir.heic src/card_64x48.png
heif-enc -q 80 -t 16 -o heic_thumb.heic src/card_33x21.png
heif-enc -q 80 -o heic_icc.heic src/card_icc_32x24.png

# Apple's encoder (macOS sips): a single image, a 512x512-tiled grid, 10 bit,
# alpha, AVIF, and EXIF orientation 6 stored next to the normative irot.
magick src/card_64x48.png -strip -quality 95 src/card_64x48.jpg
cp src/card_64x48.jpg card_64x48.jpg
python3 -I "$1/add_exif.py" src/card_64x48.jpg src/card_exif6.jpg 6
sips -s format heic -s formatOptions 80 src/card_64x48.png --out heic_sips.heic >/dev/null
sips -s format heic -s formatOptions 50 src/card_1030x520.png --out heic_sips_grid.heic >/dev/null
sips -s format heic src/ramp16_48x32.png --out heic_sips_10bit.heic >/dev/null
sips -s format heic src/alpha_48x32.png --out heic_sips_alpha.heic >/dev/null
sips -s format heic src/card_exif6.jpg --out heic_sips_exif6.heic >/dev/null
sips -s format avif src/card_64x48.png --out avif_sips.avif >/dev/null

# Reference decodes by libheif (the independent decoder the tests compare with).
for f in *.avif *.heic; do
  heif-dec "$f" "ref_${f%.*}.png" >/dev/null
done
# Only a 160x120 window (where four 512x512 tiles meet) of the large grid is kept.
magick ref_heic_sips_grid.png -crop 160x120+440+400 +repage -strip ref_heic_sips_grid_crop.png
rm ref_heic_sips_grid.png

# libavif 1.0.4 (Ubuntu 24.04 package libavif-bin) in a throwaway container:
# an avifenc animation and avifdec reference decodes (libavif interpolates
# 4:2:0 chroma, libheif repeats it).
docker run --rm --memory 2g -v "$PWD:/w" -w /w ubuntu:24.04 sh -c '
  apt-get update -qq >/dev/null && apt-get install -y -qq libavif-bin >/dev/null 2>&1 &&
  avifenc -q 70 --timescale 1000 --duration 100 -s 8 src/seq_1.png src/seq_2.png src/seq_3.png avif_anim_avifenc.avif &&
  avifdec --index 1 avif_anim_avifenc.avif avifdec_avif_anim_avifenc_1.png &&
  for f in avif_420 avif_alpha avif_sips; do avifdec "$f.avif" "avifdec_$f.png"; done' >/dev/null
