# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# WKT / WKB / EWKB fixtures written by shapely (GEOS) from the synthetic
# features:  /venv/bin/python tools/wkt_fixtures.py   (run in the geo fixture directory)
import json

import shapely
from shapely.geometry import shape

src = json.load(open("synthetic_features.geojson", encoding="utf-8"))
geoms = [shape(f["geometry"]) for f in src["features"]]

with open("wkt/features.wkt", "w") as out:
    for g in geoms:
        out.write(shapely.to_wkt(g, rounding_precision=-1) + "\n")

# 3D variants: elevation = (longitude + 30) * 1000, rounded to 4 decimals
def with_z(coords):
    if coords and isinstance(coords[0], (int, float)):
        return [coords[0], coords[1], round((coords[0] + 30) * 1000, 4)]
    return [with_z(c) for c in coords]


geoms_z = [shape({"type": f["geometry"]["type"], "coordinates": with_z(f["geometry"]["coordinates"])})
           for f in src["features"]]
with open("wkt/features_z.wkt", "w") as out:
    for g in geoms_z:
        out.write(shapely.to_wkt(g, rounding_precision=-1, output_dimension=3) + "\n")
gc = shapely.GeometryCollection([geoms[0], geoms[2], shapely.GeometryCollection([geoms[3]])])
with open("wkt/collection.wkt", "w") as out:
    out.write(shapely.to_wkt(gc, rounding_precision=-1) + "\n")

with open("wkt/features_le.wkb", "wb") as out:
    for g in geoms:
        out.write(shapely.to_wkb(g, byte_order=1))
with open("wkt/features_be.wkb", "wb") as out:
    for g in geoms:
        out.write(shapely.to_wkb(g, byte_order=0))
with open("wkt/features_z_iso.wkb", "wb") as out:
    for g in geoms_z:
        out.write(shapely.to_wkb(g, byte_order=1, flavor="iso", output_dimension=3))
with open("wkt/features_ewkb.hex", "w") as out:
    for g in geoms_z:
        out.write(shapely.to_wkb(shapely.set_srid(g, 4326), hex=True, include_srid=True, output_dimension=3) + "\n")
