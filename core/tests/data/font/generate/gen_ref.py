"""Reference data + FreeType renders. Run inside cvt-fonttools:1, cwd=/w."""
import json
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import RecordingPen
from PIL import Image, ImageDraw, ImageFont, features
import PIL, fontTools

O = "out/"
CHARS = ["A", "O", "g", "a", "À"]

def dump(obj, path):
    with open(path, "w") as fh:
        json.dump(obj, fh, separators=(",", ":"), sort_keys=False, ensure_ascii=True)
        fh.write("\n")

# ---- a. TrueType reference ----
f = TTFont(O + "inter_subset.ttf")
order = f.getGlyphOrder()
gid = {n: i for i, n in enumerate(order)}
glyf = f["glyf"]
cmap = f.getBestCmap()
ref = {
    "file": "inter_subset.ttf",
    "generator": "fontTools %s" % fontTools.version,
    "numGlyphs": f["maxp"].numGlyphs,
    "unitsPerEm": f["head"].unitsPerEm,
    "glyphOrder": order,
    "cmap": {str(cp): [n, gid[n]] for cp, n in sorted(cmap.items())},
    "advances": [f["hmtx"][n][0] for n in order],
    "lsbs": [f["hmtx"][n][1] for n in order],
    "names": {str(r.nameID): r.toUnicode() for r in f["name"].names
              if r.platformID == 3 and r.platEncID == 1 and r.langID == 0x409},
    "bboxes": [],
    "glyphs": {},
}
for n in order:
    g = glyf[n]
    ref["bboxes"].append([g.xMin, g.yMin, g.xMax, g.yMax] if g.numberOfContours != 0 else None)
for ch in CHARS:
    cp = ord(ch)
    if cp not in cmap:
        continue
    n = cmap[cp]
    g = glyf[n]
    coords, ends, flags = g.getCoordinates(glyf)
    contours, start = [], 0
    for e in ends:
        contours.append([[int(coords[i][0]), int(coords[i][1]), int(flags[i] & 1)] for i in range(start, e + 1)])
        start = e + 1
    ref["glyphs"]["U+%04X" % cp] = {
        "char": ch, "name": n, "gid": gid[n], "advance": f["hmtx"][n][0],
        "composite": bool(g.isComposite()),
        "components": [[c.glyphName, gid[c.glyphName], c.x, c.y] for c in g.components] if g.isComposite() else [],
        "bbox": [g.xMin, g.yMin, g.xMax, g.yMax],
        "contours": contours,
    }
ref["bboxes_note"] = "null = empty glyph (no contours); composite bboxes as stored in the glyf table"
ref["contours_note"] = "points [x, y, onCurve(1|0)] from glyph.getCoordinates(glyfTable) (composites flattened); TrueType quadratic, implied on-curve points not inserted"

# render placement (also documented in SOURCES.md)
PIL_INFO = "Pillow %s, FreeType %s" % (PIL.__version__, features.version("freetype2"))
renders = []
def render(path, size, text, canvas, origin):
    font = ImageFont.truetype(O + "inter_subset.ttf", size, layout_engine=ImageFont.Layout.BASIC)
    im = Image.new("L", canvas, 0)
    d = ImageDraw.Draw(im)
    d.text(origin, text, font=font, fill=255, anchor="ls")
    bbox = d.textbbox(origin, text, font=font, anchor="ls")
    assert bbox[0] >= 0 and bbox[1] >= 0 and bbox[2] <= canvas[0] and bbox[3] <= canvas[1], bbox
    im.save(O + path, optimize=False)
    renders.append({
        "file": path, "text": text, "size_px": size, "canvas": list(canvas), "mode": "L",
        "anchor": "ls", "pen_x": origin[0], "baseline_y": origin[1],
        "layout_engine": "BASIC", "ink_bbox": list(bbox),
        "pen_positions": [[c, round(origin[0] + font.getlength(text[:i]), 4)] for i, c in enumerate(text)],
        "total_advance": font.getlength(text),
    })
render("render_inter_A_64.png", 64, "A", (96, 96), (24, 72))
render("render_inter_text_32.png", 32, "Hamburgefonstiv 0123", (400, 48), (8, 36))
ref["renders"] = {"engine": PIL_INFO, "items": renders}
dump(ref, O + "inter_subset_ref.json")

# ---- b. CFF reference ----
f = TTFont(O + "inter_subset.otf")
order = f.getGlyphOrder()
gid = {n: i for i, n in enumerate(order)}
cmap = f.getBestCmap()
gs = f.getGlyphSet()
oref = {
    "file": "inter_subset.otf",
    "generator": "fontTools %s" % fontTools.version,
    "numGlyphs": f["maxp"].numGlyphs,
    "unitsPerEm": f["head"].unitsPerEm,
    "glyphOrder": order,
    "cmap": {str(cp): [n, gid[n]] for cp, n in sorted(cmap.items())},
    "advances": [f["hmtx"][n][0] for n in order],
    "glyphs": {},
    "outline_note": "fontTools RecordingPen on the CFF glyph set: [op, [[x,y],...]]; ops moveTo/lineTo/curveTo/closePath",
}
for ch in CHARS:
    cp = ord(ch)
    if cp not in cmap:
        continue
    n = cmap[cp]
    p = RecordingPen()
    gs[n].draw(p)
    oref["glyphs"]["U+%04X" % cp] = {
        "char": ch, "name": n, "gid": gid[n], "advance": f["hmtx"][n][0],
        "commands": [[op, [[float(x) if x != int(x) else int(x), float(y) if y != int(y) else int(y)] for x, y in pts]] for op, pts in p.value],
    }
dump(oref, O + "inter_subset_otf_ref.json")
print(json.dumps(renders, indent=1)[:3000])
