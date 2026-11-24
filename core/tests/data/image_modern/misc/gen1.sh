#!/bin/bash
# Source pictures for ora/ and ani/ (ImageMagick 7). Run in a scratch
# directory, then: python3 make_containers.py . <output dir>
set -e
X="-depth 8 -define png:exclude-chunks=date,time,tIME"
magick -size 48x32 gradient:'#ff0000-#0000ff' $X l0.png
magick -size 20x12 xc:'#00ff0080' $X l1.png
magick -size 16x16 xc:none -fill '#ffff00' -draw 'circle 8,8 8,2' $X l2.png
magick -size 48x32 xc:black $X l3.png
magick l0.png \( l1.png -channel A -evaluate multiply 0.5 +channel \) -geometry +10+8 -composite l2.png -geometry +30+14 -composite $X ora_expected.png
magick -size 24x16 pattern:checkerboard $X kra_merged.png
magick -size 12x8 xc:'#336699' $X kra_preview.png
for i in 0 1 2; do
  magick -size 32x32 xc:none -fill "rgb($((i*100)),$((255-i*100)),60)" -draw "rectangle $((i*8)),4 $((i*8+12)),28" $X f$i.png
  magick f$i.png f$i.ico
  magick f$i.png f$i.cur
done
magick -size 32x32 gradient:'#ff000040-#0000ffff' $X f2.png
magick f2.png -type TrueColorAlpha f2.ico
magick f2.png -type TrueColorAlpha f2.cur
magick -size 40x30 gradient:'#00ff00-#ff00ff' $X p0.png
magick -size 40x30 pattern:checkerboard -depth 8 -colors 2 $X p1.png
magick -size 40x30 plasma:fractal -seed 3 $X p2.png
magick p0.png p1.png p2.png pages.dcx
