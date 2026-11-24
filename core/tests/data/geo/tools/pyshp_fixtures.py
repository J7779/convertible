# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Shapefiles with Z, M and MultiPatch shapes written by pyshp:
#   /venv/bin/python pyshp_fixtures.py <output-directory>
# pointz:      Alpha Beacon (-29.4812345, 40.1234567, z 12.5, m 1.5)
# polylinez:   Charlie Path, z = 10 * vertex index, m = vertex index
# polygonm:    Delta Field outer ring + hole, m = vertex index (no z)
# multipatch:  a triangle strip (4 vertices, z 5) and an outer ring (square, z 0)
import sys

import shapefile

out = sys.argv[1]

w = shapefile.Writer(f"{out}/pointz", shapeType=shapefile.POINTZ)
w.field("name", "C", size=32)
w.field("rank", "N", size=6)
w.pointz(-29.4812345, 40.1234567, 12.5, 1.5)
w.record("Alpha Beacon", 3)
w.close()

w = shapefile.Writer(f"{out}/polylinez", shapeType=shapefile.POLYLINEZ)
w.field("name", "C", size=32)
pts = [(-29.49, 40.10), (-29.48, 40.11), (-29.47, 40.105), (-29.46, 40.12)]
w.linez([[[x, y, 10.0 * i, float(i)] for i, (x, y) in enumerate(pts)]])
w.record("Charlie Path")
w.close()

w = shapefile.Writer(f"{out}/polygonm", shapeType=shapefile.POLYGONM)
w.field("name", "C", size=32)
outer = [(-29.50, 40.00), (-29.50, 40.08), (-29.40, 40.08), (-29.40, 40.00), (-29.50, 40.00)]  # clockwise
hole = [(-29.47, 40.02), (-29.43, 40.02), (-29.43, 40.05), (-29.47, 40.05), (-29.47, 40.02)]  # counter-clockwise
w.polym([[[x, y, float(i % 4)] for i, (x, y) in enumerate(outer)], [[x, y, float(i % 4)] for i, (x, y) in enumerate(hole)]])
w.record("Delta Field")
w.close()

w = shapefile.Writer(f"{out}/multipatch", shapeType=shapefile.MULTIPATCH)
w.field("name", "C", size=32)
strip = [[-29.30, 40.00, 5.0], [-29.25, 40.00, 5.0], [-29.30, 40.04, 5.0], [-29.25, 40.04, 5.0]]
ring = [[-29.20, 40.00, 0.0], [-29.20, 40.04, 0.0], [-29.15, 40.04, 0.0], [-29.15, 40.00, 0.0], [-29.20, 40.00, 0.0]]
w.multipatch([strip, ring], partTypes=[shapefile.TRIANGLE_STRIP, shapefile.OUTER_RING])
w.record("Patch")
w.close()
