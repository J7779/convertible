# CAD and plotter fixtures (DXF, HP-GL, CGM)

Synthetic drawings only (shapes, "Lorem", "Hello DXF"). Tools ran in a
throwaway Ubuntu 24.04 container; commands use relative paths. Scripts
quoted below were run with `/venv/bin/python -I make_dxf.py .` and `sh plot.sh`.

## DXF written by ezdxf 1.3.4

`ezdxf.options.write_fixed_meta_data_for_testing = True` (fixed dates,
handle seed and GUIDs; `$LASTSAVEDBY` is "ezdxf"). All drawings use
`ezdxf.new(version, setup=False)` plus a DASHED line type
(`pattern=[0.75, 0.5, -0.25]`).

| File | Content |
| --- | --- |
| `common_r12.dxf`, `common_r2000.dxf`, `common_r2004.dxf`, `common_r2007.dxf`, `common_r2010.dxf`, `common_r2013.dxf`, `common_r2018.dxf` | The same drawing saved as R12 (AC1009), 2000, 2004, 2007, 2010, 2013 and 2018 (AC1032); `$INSUNITS` 4 (mm, not stored in R12). Layers WALLS (ACI 1, 0.50 mm), DASHED_LAYER (ACI 5, DASHED), OFF_LAYER (off), FROZEN_LAYER (frozen). LINE (0,0)-(100,0) on WALLS; closed polyline (0,10) (40,10 bulge 1) (40,30) (0,30) on WALLS (LWPOLYLINE, POLYLINE in R12); CIRCLE (70,20) r 10 ACI 3; ARC (120,20) r 15 0..90 deg on DASHED_LAYER; TEXT "Hello DXF" at (0,80) height 5; INSERT of block MARK (two BYBLOCK lines +-5, CIRCLE r 3 ACI 3) at (200,20) scale 2 rotation 45 ACI 6; SOLID (100,100) (110,100) (100,110) (110,110) ACI 2; POINT (5,5); one LINE on each hidden layer. |
| `common_r12_binary.dxf`, `common_r2010_binary.dxf` | The R12 and 2010 drawings saved with `doc.saveas(path, fmt="bin")` (binary DXF; 8-bit and 16-bit group codes). |
| `rich_r2018.dxf` | 2018, mm. ELLIPSE (150,20) major (20,0) ratio 0.5; open cubic SPLINE with control points (0,50) (10,70) (30,40) (50,60) (60,50), knots 0 0 0 0 1 2 2 2 2 (ezdxf evaluates (0,50) (15,59.6875) (30,52.5) (45,54.0625) (60,50) at u = 0, 0.5, 1, 1.5, 2); rational quadratic SPLINE (80,50) (90,70 w=2) (100,50) (midpoint (90, 63.3333)); fit-point SPLINE through (0,160) (20,175) (40,160) (60,175); TEXT "Middle" MIDDLE_CENTER at (100,80) rotated 30; MTEXT `First line\PSecond {\C1;red} line`; MINSERT of MARK at (200,60) 3 columns x 2 rows, spacing 15 / 10; solid HATCH (60..80 square, ACI 2); HATCH with an edge path of lines and an arc reaching (150,70) (ACI 4); ANSI31 pattern HATCH 160..180 (ACI 7); LINE in true colour 0x123456; LINE with line weight 1.00 mm; block TAG with ATTDEF NAME and an INSERT carrying ATTRIB "Pump 7"; rendered linear DIMENSION (text "100"); LEADER; a LINE (0,0)-(30,30) ACI 1 in paper space Layout1; nested blocks OUTER -> INNER (rotated 90) at (300,0); CIRCLE at OCS (10,10) with extrusion (0,0,-1). |
| `cp1251_r2000.dxf` | 2000, `$DWGCODEPAGE` ANSI_1251, `$INSUNITS` 6 (m); TEXT "Привет Ω" stored in cp1251 with Ω as `\U+03A9`; a LINE. Saved with `encoding="cp1251"`. |
| `utf8_inches_r2018.dxf` | 2018 (UTF-8), `$INSUNITS` 1 (inch); TEXT "Grüße ⌀ %%c10 %%d %%p"; MTEXT `A\~B \\ \{x\} \S1/2; \LUnder\l \fArial|b1;Bold`; a LINE. |
| `mesh3d_r2018.dxf` | 2018 (`$INSUNITS` 6, ezdxf default): 3DFACE quad (0,0,0)-(10,10,0), 3DFACE triangle with apex (5,0,8), polyface POLYLINE of `forms.cube()` moved by (20,0,0), 3x3 polygon-mesh POLYLINE at x 40..50, MESH of a cube scaled 4 at x 60, a LINE. 18 faces in all. |
| `mesh3d_r12.dxf` | R12: one 3DFACE triangle and a one-face polyface POLYLINE. |

The generator, run as `/venv/bin/python -I make_dxf.py .` (it also writes
`reference.json` with the ezdxf-evaluated spline and bulge values used by the
tests; that file is not committed):

```python
"""Generate the DXF fixtures with ezdxf (run inside the tools container)."""
import json
import math
import sys

import ezdxf
from ezdxf import bbox
from ezdxf.math import bulge_to_arc
from ezdxf.render import forms

ezdxf.options.write_fixed_meta_data_for_testing = True

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
ref = {}


def setup_layers(doc):
    if "DASHED" not in doc.linetypes:
        doc.linetypes.add("DASHED", pattern=[0.75, 0.5, -0.25], description="Dashed __ __ __")
    doc.layers.add("WALLS", color=1, linetype="CONTINUOUS", lineweight=50)
    doc.layers.add("DASHED_LAYER", color=5, linetype="DASHED")
    off = doc.layers.add("OFF_LAYER", color=4)
    off.off()
    frz = doc.layers.add("FROZEN_LAYER", color=4)
    frz.freeze()


def mark_block(doc):
    blk = doc.blocks.new("MARK")
    blk.add_line((-5, 0), (5, 0), dxfattribs={"color": 0})  # BYBLOCK
    blk.add_line((0, -5), (0, 5), dxfattribs={"color": 0})
    blk.add_circle((0, 0), 3, dxfattribs={"color": 3})


def common(doc, lw=True):
    """Entities every DXF version can hold."""
    msp = doc.modelspace()
    msp.add_line((0, 0), (100, 0), dxfattribs={"layer": "WALLS"})
    pts = [(0, 10, 0), (40, 10, 1.0), (40, 30, 0), (0, 30, 0)]
    if lw:
        msp.add_lwpolyline(pts, format="xyb", close=True, dxfattribs={"layer": "WALLS"})
    else:
        msp.add_polyline2d([(x, y) for x, y, _ in pts], close=True, dxfattribs={"layer": "WALLS"})
        pl = msp.query("POLYLINE").last
        for v, (_, _, b) in zip(pl.vertices, pts):
            v.dxf.bulge = b
    msp.add_circle((70, 20), 10, dxfattribs={"color": 3})
    msp.add_arc((120, 20), 15, 0, 90, dxfattribs={"layer": "DASHED_LAYER"})
    msp.add_text("Hello DXF", height=5, dxfattribs={"insert": (0, 80)})
    msp.add_blockref("MARK", (200, 20), dxfattribs={"xscale": 2, "yscale": 2, "rotation": 45, "color": 6})
    msp.add_solid([(100, 100), (110, 100), (100, 110), (110, 110)], dxfattribs={"color": 2})
    msp.add_point((5, 5))
    msp.add_line((0, 140), (10, 140), dxfattribs={"layer": "OFF_LAYER"})
    msp.add_line((0, 150), (10, 150), dxfattribs={"layer": "FROZEN_LAYER"})


def version_matrix():
    for ver, name in [("R12", "r12"), ("R2000", "r2000"), ("R2004", "r2004"), ("R2007", "r2007"),
                      ("R2010", "r2010"), ("R2013", "r2013"), ("R2018", "r2018")]:
        doc = ezdxf.new(ver, setup=False)
        doc.header["$INSUNITS"] = 4
        setup_layers(doc)
        mark_block(doc)
        common(doc, lw=ver != "R12")
        doc.saveas(f"{OUT}/common_{name}.dxf")
        if ver in ("R12", "R2010"):
            doc.saveas(f"{OUT}/common_{name}_binary.dxf", fmt="bin")


def rich():
    doc = ezdxf.new("R2018", setup=False)
    doc.header["$INSUNITS"] = 4
    setup_layers(doc)
    mark_block(doc)
    msp = doc.modelspace()
    msp.add_ellipse((150, 20), major_axis=(20, 0), ratio=0.5)
    cps = [(0, 50), (10, 70), (30, 40), (50, 60), (60, 50)]
    sp = msp.add_open_spline(cps, degree=3)
    tool = sp.construction_tool()
    ref["spline_cp"] = [list(tool.point(t))[:2] for t in (0.0, 0.5, 1.0, 1.5, 2.0)]
    ref["spline_cp_t"] = [0.0, 0.5, 1.0, 1.5, 2.0]
    ref["spline_cp_knots"] = list(tool.knots())
    w = msp.add_rational_spline([(80, 50), (90, 70), (100, 50)], weights=[1, 2, 1], degree=2)
    wt = w.construction_tool()
    ref["spline_rational_mid"] = list(wt.point(0.5))[:2]; ref["rational_knots"] = list(wt.knots())
    fit = [(0, 160), (20, 175), (40, 160), (60, 175)]
    msp.add_spline(fit)
    ref["spline_fit"] = fit
    msp.add_text("Middle", height=4, rotation=30, dxfattribs={"insert": (100, 80)}).set_placement(
        (100, 80), align=ezdxf.enums.TextEntityAlignment.MIDDLE_CENTER)
    msp.add_mtext(r"First line\PSecond {\C1;red} line", dxfattribs={"insert": (0, 100), "char_height": 4})
    msp.add_blockref("MARK", (200, 60), dxfattribs={"column_count": 3, "row_count": 2, "column_spacing": 15,
                                                     "row_spacing": 10})
    h = msp.add_hatch(color=2)
    h.paths.add_polyline_path([(60, 60), (80, 60), (80, 80), (60, 80)], is_closed=True)
    h2 = msp.add_hatch(color=4)
    ep = h2.paths.add_edge_path()
    ep.add_line((120, 60), (140, 60))
    ep.add_arc((140, 70), 10, -90, 90)
    ep.add_line((140, 80), (120, 80))
    ep.add_line((120, 80), (120, 60))
    h3 = msp.add_hatch(color=1)
    h3.set_pattern_fill("ANSI31", scale=1.0)
    h3.paths.add_polyline_path([(160, 60), (180, 60), (180, 80), (160, 80)], is_closed=True)
    msp.add_line((0, 120), (50, 120), dxfattribs={"true_color": ezdxf.colors.rgb2int((0x12, 0x34, 0x56))})
    msp.add_line((0, 125), (50, 125), dxfattribs={"lineweight": 100})
    att = doc.blocks.new("TAG")
    att.add_circle((0, 0), 4)
    att.add_attdef("NAME", (5, 0), dxfattribs={"height": 3})
    msp.add_auto_blockref("TAG", (250, 100), {"NAME": "Pump 7"})
    dim = msp.add_linear_dim(base=(0, -10), p1=(0, 0), p2=(100, 0))
    dim.render()
    msp.add_leader([(200, 140), (220, 150), (240, 150)])
    psp = doc.paperspace("Layout1")
    psp.add_line((0, 0), (30, 30), dxfattribs={"color": 1})
    # nested blocks
    inner = doc.blocks.new("INNER")
    inner.add_line((0, 0), (10, 0))
    outer = doc.blocks.new("OUTER")
    outer.add_blockref("INNER", (0, 0), dxfattribs={"rotation": 90})
    outer.add_blockref("INNER", (20, 0))
    msp.add_blockref("OUTER", (300, 0))
    # OCS: a circle with extrusion (0,0,-1) mirrors x
    msp.add_circle((10, 10), 2, dxfattribs={"extrusion": (0, 0, -1)})
    doc.saveas(f"{OUT}/rich_r2018.dxf")
    # reference geometry
    c, a0, a1, r = bulge_to_arc((40, 10), (40, 30), 1.0); ref["bulge"] = [c.x, c.y, a0, a1, r]
    ref["rich_extents"] = None


def codepages():
    doc = ezdxf.new("R2000")
    doc.header["$DWGCODEPAGE"] = "ANSI_1251"
    doc.header["$INSUNITS"] = 6  # metres
    msp = doc.modelspace()
    msp.add_text("Привет Ω", height=1, dxfattribs={"insert": (0, 0)})
    msp.add_line((0, 0), (2, 1))
    doc.encoding = "cp1251"
    doc.saveas(f"{OUT}/cp1251_r2000.dxf", encoding="cp1251")
    doc = ezdxf.new("R2018")
    doc.header["$INSUNITS"] = 1  # inches
    msp = doc.modelspace()
    msp.add_text("Grüße ⌀ %%c10 %%d %%p", height=0.25, dxfattribs={"insert": (1, 1)})
    msp.add_mtext(r"A\~B \\ \{x\} \S1/2; \LUnder\l \fArial|b1;Bold", dxfattribs={"insert": (1, 2), "char_height": 0.2})
    msp.add_line((0, 0), (3, 2))
    doc.saveas(f"{OUT}/utf8_inches_r2018.dxf")


def mesh3d():
    doc = ezdxf.new("R2018")
    msp = doc.modelspace()
    msp.add_3dface([(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)])
    msp.add_3dface([(0, 0, 0), (10, 0, 0), (5, 0, 8), (5, 0, 8)])  # triangle (4th == 3rd)
    pf = msp.add_polyface()
    cube = forms.cube().translate(20, 0, 0)
    pf.append_faces([[cube.vertices[i] for i in f] for f in cube.faces])
    pf.optimize()
    pm = msp.add_polymesh(size=(3, 3))
    for m in range(3):
        for n in range(3):
            pm.set_mesh_vertex((m, n), (40 + m * 5, n * 5, (m * n) % 2))
    mesh = msp.add_mesh()
    c2 = forms.cube().scale_uniform(4).translate(60, 0, 0)
    with mesh.edit_data() as data:
        data.vertices = list(c2.vertices)
        data.faces = list(c2.faces)
    msp.add_line((0, -5, 0), (10, -5, 0))
    doc.saveas(f"{OUT}/mesh3d_r2018.dxf")
    r12 = ezdxf.new("R12")
    m12 = r12.modelspace()
    m12.add_3dface([(0, 0, 0), (1, 0, 0), (1, 1, 1)])
    pf = m12.add_polyface()
    pf.append_faces([[(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]])
    r12.saveas(f"{OUT}/mesh3d_r12.dxf")


version_matrix()
rich()
codepages()
mesh3d()
json.dump(ref, open(f"{OUT}/reference.json", "w"), indent=1)
```

## DXF and HP-GL written by pstoedit 4.00 (with Ghostscript 10)

`shapes.ps`: a red stroked triangle (72,72) (216,72) (216,144), a blue filled
circle at (300,108) r 36, "Lorem" in Helvetica 24 at (72,200).

| File | Command |
| --- | --- |
| `pstoedit_dxf14.dxf` | `pstoedit -q -f dxf_14 shapes.ps pstoedit_dxf14.dxf` (AC1014, inches) |
| `pstoedit_dxf9.dxf` | `pstoedit -q -f dxf shapes.ps pstoedit_dxf9.dxf` (no version header) |
| `pstoedit.hpgl` | `pstoedit -q -f hpgl shapes.ps pstoedit.hpgl` (LB label with DI/SI, FP fill) |

## HP-GL, PCL and CGM written by GNU plotutils 2.6 (libplot 4.4)

`shapes.pic`:

```
.PS
box wid 1 ht 0.5 "Box"
arrow right 0.5
circle rad 0.25
line right 0.5 then down 0.5
ellipse wid 0.8 ht 0.4 at (1,-1)
arc cw rad 0.3 from (2.5,-1) to (3.1,-1)
spline from (0,-1.5) to (0.5,-1) then to (1,-1.5) then to (1.5,-1)
"Lorem" at (2,-1.7)
.PE
```

`d.txt`: the points `0 0`, `1 1`, `2 0.5`, `3 2`, `4 1`, one per line.

| File | Command |
| --- | --- |
| `shapes_hpgl2.hpgl` | `pic2plot -T hpgl shapes.pic` (HP-GL/2: PM/FP/EP, CI, AA, PW with WU1, Hershey text as strokes) |
| `shapes_hpgl15.hpgl` | `HPGL_VERSION=1.5 pic2plot -T hpgl shapes.pic` |
| `shapes_hpgl1.hpgl` | `HPGL_VERSION=1 pic2plot -T hpgl shapes.pic` |
| `shapes_pcl5.pcl` | `pic2plot -T pcl shapes.pic` (HP-GL/2 inside PCL 5: ESC%0B ... ESC%0A, BZ Béziers, LB labels) |
| `graph_hpgl2.hpgl` | `graph -T hpgl --pen-colors 1=red:2=green --toggle-use-color -m 2 d.txt` (data curve on pen 3) |
| `shapes_binary.cgm` | `CGM_ENCODING=binary pic2plot -T cgm shapes.pic` |
| `shapes_clear.cgm` | `CGM_ENCODING=clear_text pic2plot -T cgm shapes.pic` (VDCEXT -8191..8191, SCALEMODE metric 0.01240386; RECT (-3174,973)-(-1126,1997), CIRCLE (410,1485) 512, ARCCTR (2560,-563) r 615, ELLIPSE, POLYBEZIER, RESTRTEXT "Box" and "Lorem" with CHARHEIGHT 204) |
| `graph_binary.cgm`, `graph_clear.cgm` | `CGM_ENCODING=binary graph -T cgm --toggle-use-color d.txt` and the same with `clear_text` (red data line, LINECOLR 65535 0 0) |

Post-processing: libplot writes the current date into the CGM metafile
description; `Date:YYYYMMDD` was replaced by `Date:20000101` (same length) in
all four CGM files with a byte-level search and replace.

## Interop checks of our output

- `ezdxf audit` (ezdxf 1.3.4, `doc.audit()` after `ezdxf.readfile`): our DXF
  written from `rich_r2018.dxf`, `utf8_inches_r2018.dxf`, `cp1251_r2000.dxf`,
  `shapes_clear.cgm` and `shapes_hpgl2.hpgl` in each of `version=r12`, `2000`
  and `2018` (15 files), plus `mesh3d_r2018.dxf` -> dxf: 0 errors, 0 fixes in
  every file; entity counts as expected (LWPOLYLINE, SPLINE, ELLIPSE, CIRCLE,
  TEXT, HATCH; POLYLINE/CIRCLE/TEXT for R12).
- LibreOffice 24.2 (`soffice --headless --convert-to png`) opens our R12,
  2000 and 2018 files written from `rich_r2018.dxf` and `shapes_clear.cgm` and
  draws layers, colours, text, solid hatches and polylines. Its DXF import
  does not draw SPLINE and ELLIPSE entities (they are missing from the 2000
  and 2018 renderings; the R12 file, where curves are flattened, is complete).
- Inkscape 1.2 could not be used headless for HP-GL import or DXF export in
  the container (the commands hang or fail), so our HP-GL output is checked
  by our own reader only.
