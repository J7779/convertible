# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Encoded polylines (python "polyline"), geohashes (pygeohash) and Plus Codes
# (openlocationcode) of the synthetic data:
#   /venv/bin/python tools/code_fixtures.py   (run in the geo fixture directory)
import sys

import polyline
import pygeohash
from openlocationcode import openlocationcode as olc

sys.path.insert(0, "tools")
import synthetic  # noqa: E402

charlie = [(40.10, -29.49), (40.11, -29.48), (40.105, -29.47), (40.12, -29.46)]
seg1 = [(round(p["lat"], 7), round(p["lon"], 7)) for p in synthetic.SEG1]
with open("polyline/lines.polyline", "w") as out:
    out.write(polyline.encode(charlie, 5) + "\n" + polyline.encode(seg1, 5) + "\n")
with open("polyline/lines.polyline6", "w") as out:
    out.write(polyline.encode(charlie, 6) + "\n" + polyline.encode(seg1, 6) + "\n")

points = [(40.1234567, -29.4812345), (40.15, -29.45), (40.30, -29.10)]
with open("codes/points.geohash", "w") as out:
    for lat, lon in points:
        out.write(pygeohash.encode(lat, lon, precision=11) + "\n")
with open("codes/points_short.geohash", "w") as out:
    for lat, lon in points:
        out.write(pygeohash.encode(lat, lon, precision=6) + "\n")
with open("codes/points.pluscode", "w") as out:
    for lat, lon in points:
        out.write(olc.encode(lat, lon, 11) + "\n")
