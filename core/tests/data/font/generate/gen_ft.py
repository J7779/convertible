"""fontTools-based fixture generation. Run inside cvt-fonttools:1 with cwd=/w (relative paths)."""
import json, os
from fontTools.ttLib import TTFont, TTCollection
from fontTools.ttLib.woff2 import compress as woff2_compress
from fontTools.ttLib.sfnt import WOFFFlavorData
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.cffLib.CFFToCFF2 import convertCFFToCFF2

O = "out/"

# ---- 3. TrueType -> CFF OTF (cubic outlines, contours reversed to CFF direction) ----
def ttf_to_otf(src_path, dst_path):
    src = TTFont(src_path)
    order = src.getGlyphOrder()
    gs = src.getGlyphSet()
    hmtx = src["hmtx"].metrics
    charstrings = {}
    for name in order:
        pen = T2CharStringPen(hmtx[name][0], gs)
        gs[name].draw(ReverseContourPen(pen))
        charstrings[name] = pen.getCharString()
    names = {r.nameID: r.toUnicode() for r in src["name"].names if r.platformID == 3}
    fb = FontBuilder(src["head"].unitsPerEm, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCFF(names[6], {"FullName": names[4], "FamilyName": names[1], "Weight": names[2],
                           "version": "%.3f" % src["head"].fontRevision, "Notice": names[0]},
                charstrings, {})
    for tag in ["head", "hhea", "hmtx", "cmap", "name", "OS/2", "GDEF", "GPOS", "GSUB"]:
        if tag in src:
            fb.font[tag] = src[tag]
    fb.setupMaxp()
    fb.setupPost(keepGlyphNames=False)
    p = fb.font["post"]
    for a in ("italicAngle", "underlinePosition", "underlineThickness", "isFixedPitch"):
        setattr(p, a, getattr(src["post"], a))
    fb.font.recalcTimestamp = False
    fb.save(dst_path)

ttf_to_otf(O + "inter_subset.ttf", O + "inter_subset.otf")

# ---- 4. CFF -> CFF2 ----
f = TTFont(O + "inter_subset.otf", recalcTimestamp=False)
convertCFFToCFF2(f)
f.save(O + "inter_subset_cff2.otf")

# ---- 5. bare CFF table ----
f = TTFont(O + "inter_subset.otf")
with open(O + "inter_subset.cff", "wb") as fh:
    fh.write(f.reader["CFF "])

# ---- 6. WOFF 1 ----
f = TTFont(O + "inter_subset.ttf", recalcTimestamp=False)
f.flavor = "woff"
f.save(O + "inter_subset.woff")
f = TTFont(O + "inter_subset.ttf", recalcTimestamp=False)
f.flavor = "woff"
fd = WOFFFlavorData()
fd.metaData = (b'<?xml version="1.0" encoding="UTF-8"?><metadata version="1.0"><description>'
               b'<text lang="en">Test font</text></description></metadata>')
fd.privData = b"private-test"
f.flavorData = fd
f.save(O + "inter_meta.woff")

# ---- 7. WOFF2 via fontTools ----
woff2_compress(O + "inter_subset.ttf", O + "inter_subset_ft.woff2")
woff2_compress(O + "jbmono_subset.ttf", O + "jbmono_hmtx.woff2", transform_tables={"glyf", "loca", "hmtx"})

# ---- 8. TTC ----
c = TTCollection()
c.fonts = [TTFont(O + "inter_subset.ttf", recalcTimestamp=False), TTFont(O + "jbmono_subset.ttf", recalcTimestamp=False)]
c.save(O + "pair.ttc", shareTables=True)

# ---- 15. Mac 'true' sfnt version ----
f = TTFont(O + "inter_subset.ttf", recalcTimestamp=False)
f.sfntVersion = "true"
f.save(O + "inter_mac_true.ttf")

# ---- 14. tiny variable font (based on the fontTools FontBuilder docs example) ----
def draw_rect(pen, x0, y0, x1, y1):
    pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0)); pen.closePath()

def draw_A(pen, stem):
    # simple triangle-ish "A" with a counter, stem = stroke thickness
    pen.moveTo((0, 0)); pen.lineTo((250, 700)); pen.lineTo((350, 700)); pen.lineTo((600, 0))
    pen.lineTo((600 - stem, 0)); pen.lineTo((300, 640 - stem)); pen.lineTo((stem, 0)); pen.closePath()

def draw_o(pen, stem):
    # outer and inner quadratic loops
    pen.moveTo((300, 0)); pen.qCurveTo((550, 0), (550, 250)); pen.qCurveTo((550, 500), (300, 500))
    pen.qCurveTo((50, 500), (50, 250)); pen.qCurveTo((50, 0), (300, 0)); pen.closePath()
    i = 50 + stem
    pen.moveTo((300, stem)); pen.qCurveTo((i, stem), (i, 250)); pen.qCurveTo((i, 500 - stem), (300, 500 - stem))
    pen.qCurveTo((600 - i, 500 - stem), (600 - i, 250)); pen.qCurveTo((600 - i, stem), (300, stem)); pen.closePath()

def build_glyphs(stem):
    out = {}
    p = TTGlyphPen(None); draw_rect(p, 50, 0, 450, 700); draw_rect(p, 100, 50, 400, 650); out[".notdef"] = p.glyph()
    out["space"] = TTGlyphPen(None).glyph()
    p = TTGlyphPen(None); draw_A(p, stem); out["A"] = p.glyph()
    p = TTGlyphPen(None); draw_o(p, stem); out["o"] = p.glyph()
    p = TTGlyphPen(None); draw_rect(p, 100, 0, 100 + stem, 700); out["l"] = p.glyph()
    return out

order = [".notdef", "space", "A", "o", "l"]
fb = FontBuilder(1000, isTTF=True)
fb.setupGlyphOrder(order)
fb.setupCharacterMap({0x20: "space", 0x41: "A", 0x6F: "o", 0x6C: "l"})
light, bold = build_glyphs(60), build_glyphs(180)
fb.setupGlyf(light)
widths = {".notdef": 500, "space": 250, "A": 600, "o": 600, "l": 300}
def _lsb(gl):
    c = getattr(gl, "coordinates", None)
    return min(x for x, y in c) if c else 0
fb.setupHorizontalMetrics({g: (widths[g], _lsb(light[g])) for g in order})
fb.setupHorizontalHeader(ascent=800, descent=-200)
fb.setupNameTable({"familyName": "Tiny Variable", "styleName": "Regular", "uniqueFontIdentifier": "TinyVariable-Regular;1.000",
                   "fullName": "Tiny Variable Regular", "psName": "TinyVariable-Regular", "version": "Version 1.000",
                   "licenseDescription": "Synthetic test font, public domain (CC0)."})
fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200, usWeightClass=400)
fb.setupPost()
fb.setupHead(unitsPerEm=1000, created=3786825600, modified=3786825600)  # 2024-01-01T00:00:00Z
fb.setupFvar([("wght", 100, 400, 900, "Weight")],
             [dict(location=dict(wght=100), stylename="Thin"), dict(location=dict(wght=400), stylename="Regular"),
              dict(location=dict(wght=900), stylename="Black")])

# gvar: master at wght=400 is "light" (stem 60), wght=900 -> stem 180, wght=100 -> stem 30
from fontTools.ttLib.tables.TupleVariation import TupleVariation
thin = build_glyphs(30)
variations = {}
for g in order:
    if not hasattr(light[g], "coordinates") or light[g].numberOfContours <= 0:
        variations[g] = []
        continue
    base = list(light[g].coordinates)
    var = []
    for target, axis in ((bold[g], 1.0), (thin[g], -1.0)):
        deltas = [(tx - bx, ty - by) for (bx, by), (tx, ty) in zip(base, target.coordinates)]
        deltas += [(0, 0)] * 4  # phantom points
        var.append(TupleVariation({"wght": (min(0, axis), axis, max(0, axis))}, deltas))
    variations[g] = var
fb.setupGvar(variations)
fb.font.recalcTimestamp = False
fb.save(O + "tiny_variable.ttf")
