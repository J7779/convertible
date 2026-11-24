#!/bin/sh
# Runs inside cvt-fonttools:1 with cwd /w. Produces subsets in out/.
set -e
cd "$(dirname "$0")"
mkdir -p out
COMMON="--layout-features=kern,liga,calt --hinting --name-IDs=0,1,2,3,4,5,6,13,14 --name-languages=* --notdef-outline --no-recalc-timestamp --drop-tables+=FFTM,DSIG"
pyftsubset src/Inter-Regular.ttf --unicodes=U+0020-007E,U+00C0-00C5 $COMMON --output-file=out/inter_subset.ttf
pyftsubset src/JetBrainsMonoNL-Regular.ttf --unicodes=U+0020-007E $COMMON --output-file=out/jbmono_subset.ttf
python3 - <<'EOF'
from fontTools.ttLib import TTFont
for f in ['out/inter_subset.ttf','out/jbmono_subset.ttf']:
    t=TTFont(f); g=t['glyf']
    comps=[n for n in t.getGlyphOrder() if g[n].isComposite()]
    print(f, 'glyphs', t['maxp'].numGlyphs, 'tables', sorted(t.keys()), 'composites', len(comps), comps[:20])
    print(' names', [(r.platformID,r.nameID,r.toUnicode()[:60]) for r in t['name'].names])
    print(' head created', t['head'].created, t['head'].modified)
EOF
ls -l out
