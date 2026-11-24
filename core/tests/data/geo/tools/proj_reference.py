# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Reference values for the coordinate tests, computed with pyproj (PROJ):
#   /venv/bin/python tools/proj_reference.py > json/proj_reference.txt
# Each line: <target> <lon> <lat> <x> <y>, then geodesic distances and areas.
# British National Grid uses the 7-parameter Helmert transformation that the
# Convertible implementation uses (PROJ would otherwise pick a grid shift).
import json

from pyproj import CRS, Geod, Transformer

cases = [
    ("EPSG:3857", -29.4812345, 40.1234567), ("EPSG:3857", 179.5, -85.0), ("EPSG:3857", 0.0, 0.0),
    ("EPSG:32626", -29.4812345, 40.1234567), ("EPSG:32626", -27.0, 0.5), ("EPSG:32626", -33.5, 70.0),
    ("EPSG:32733", 15.2, -33.9), ("EPSG:32733", 12.01, -0.01),
    ("EPSG:25832", 9.5, 48.7), ("EPSG:26915", -93.2, 44.9),
    ("BNG", -1.5, 52.6), ("BNG", -4.2, 57.1), ("BNG", 1.3, 51.1),
]
bng = CRS.from_proj4("+proj=tmerc +lat_0=49 +lon_0=-2 +k=0.9996012717 +x_0=400000 +y_0=-100000 +ellps=airy "
                     "+towgs84=446.448,-125.157,542.06,0.1502,0.247,0.8421,-20.4894 +units=m +no_defs")
for target, lon, lat in cases:
    crs = bng if target == "BNG" else CRS.from_user_input(target)
    t = Transformer.from_crs("EPSG:4326", crs, always_xy=True)
    x, y = t.transform(lon, lat)
    print(f"{'EPSG:27700' if target == 'BNG' else target} {lon!r} {lat!r} {x:.4f} {y:.4f}")

g = Geod(ellps="WGS84")
for (lon1, lat1, lon2, lat2) in [(-29.49, 40.10, -29.48, 40.11), (0, 0, 1, 1), (-0.1, 51.5, 2.35, 48.85),
                                 (139.7, 35.7, -122.4, 37.8), (0, 0, 179.5, 0.5)]:
    _, _, d = g.inv(lon1, lat1, lon2, lat2)
    print(f"distance {lon1} {lat1} {lon2} {lat2} {d:.4f}")

src = json.load(open("synthetic_features.geojson", encoding="utf-8"))
for f in src["features"]:
    gm = f["geometry"]
    if gm["type"] in ("Polygon", "MultiPolygon"):
        from shapely.geometry import shape
        area, perimeter = g.geometry_area_perimeter(shape(gm))
        print(f"area {f['properties']['name'].replace(' ', '_')} {abs(area):.2f} {perimeter:.4f}")
