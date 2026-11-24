"""FontForge generation. Run (cwd /w, env SOURCE_DATE_EPOCH=1704067200 USER=fixture):
     fontforge -quiet -lang=py -script ff.py
"""
import fontforge, os

os.makedirs("ff", exist_ok=True)
os.makedirs("tmp", exist_ok=True)

# dfont from the TTF (keeps TrueType outlines + instructions)
f = fontforge.open("out/inter_subset.ttf")
f.generate("ff/inter_subset.dfont", flags=("no-FFTM-table", "opentype"))
f.close()

# Type 1 (PFB, PFA + AFM) and SVG font from the CFF OTF; default encoding (UnicodeBmp)
# -> FontForge writes a custom /Encoding 256 array with code = Unicode value for U+0020..00FF.
f = fontforge.open("out/inter_subset.otf")
f.generate("ff/inter_subset.pfb", flags=("afm",))
f.generate("ff/inter_subset.pfa")
f.generate("ff/inter_subset.svg")
f.close()

# Prepare sources for the bitmap strikes: decompose composites and drop unencoded glyphs
# (except .notdef), so that bitmap fonts only hold cmap-reachable glyphs.
for src, dst in (("out/inter_subset.ttf", "tmp/mini_src.sfd"), ("out/jbmono_subset.ttf", "tmp/mono_src.sfd")):
    f = fontforge.open(src)
    f.selection.all()
    f.unlinkReferences()
    for g in list(f.glyphs()):
        if g.unicode == -1 and g.glyphname != ".notdef":
            f.removeGlyph(g)
    f.weight = "Medium"  # X11 convention for the normal weight; FontForge otherwise writes "Book"
    f.save(dst)
    f.close()
print(sorted(os.listdir("ff")))
