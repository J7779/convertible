#!/bin/sh
# Assemble final fixtures in final/. Runs in container, cwd /w.
set -e
rm -rf final && mkdir final
for f in inter_subset.ttf jbmono_subset.ttf inter_subset.otf inter_subset_cff2.otf inter_subset.cff \
         inter_subset.woff inter_meta.woff inter_subset.woff2 inter_subset_ft.woff2 jbmono_hmtx.woff2 \
         pair.ttc pair.woff2 inter_subset.eot tiny_variable.ttf tiny_variable.woff2 inter_mac_true.ttf \
         inter_subset_ref.json inter_subset_otf_ref.json render_inter_A_64.png render_inter_text_32.png; do
  cp out/$f final/
done
for f in inter_subset.dfont inter_subset.pfb inter_subset.pfa inter_subset.afm inter_subset.svg \
         mini16.fon mono16.fon; do
  cp ff/$f final/
done
for b in mini16 mono16; do
  cp ff/$b-16.bdf final/$b.bdf
  cp ff/$b-16.fnt final/$b.fnt
  # FontForge always writes FFTM (+ PfEd) into .otb; drop them (fontTools re-serialisation)
  python3 -c "
import sys
from fontTools.ttLib import TTFont
t = TTFont(sys.argv[1], recalcTimestamp=False, recalcBBoxes=False)
for tag in ('FFTM', 'PfEd'):
    if tag in t: del t[tag]
t.save(sys.argv[2])
" ff/$b.otb final/$b.otb
  bdftopcf -o final/$b.pcf final/$b.bdf
done
bdf2psf --fb final/mono16.bdf /usr/share/bdf2psf/standard.equivalents /usr/share/bdf2psf/ascii.set 256 final/mono16.psf 2>/dev/null
cp src/Inter-LICENSE.txt final/OFL-Inter.txt
cp src/JetBrainsMono-LICENSE.txt final/OFL-JetBrainsMono.txt
cd final
for f in *.ttf *.otf *.woff *.woff2 *.ttc *.otb; do printf "ots %s: " $f; ots-sanitize $f >/dev/null 2>&1 && echo OK || { ots-sanitize $f 2>&1 | tail -2; }; done
file * | sed 's/^/  /'
ls -l
du -ch . | tail -1
