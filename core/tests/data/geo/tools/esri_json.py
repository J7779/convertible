# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Writes Esri JSON FeatureSets from the synthetic GeoJSON with the Python
# standard library (GDAL reads Esri JSON but cannot write it):
#   python3 esri_json.py synthetic_features.geojson out.esrijson
# out.esrijson holds the two polygon features (esriGeometryPolygon, outer
# rings clockwise as Esri requires); out_points.esrijson the two points.
import json
import sys


def signed_area(ring):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:])) / 2


def esri_rings(polygons):
    rings = []
    for polygon in polygons:
        for i, ring in enumerate(polygon):
            clockwise = signed_area(ring) < 0
            want_clockwise = i == 0
            rings.append(ring if clockwise == want_clockwise else list(reversed(ring)))
    return rings


src = json.load(open(sys.argv[1], encoding="utf-8"))
fields = [{"name": "name", "type": "esriFieldTypeString", "alias": "name", "length": 64},
          {"name": "kind", "type": "esriFieldTypeString", "alias": "kind", "length": 16},
          {"name": "rank", "type": "esriFieldTypeInteger", "alias": "rank"},
          {"name": "height", "type": "esriFieldTypeDouble", "alias": "height"},
          {"name": "label", "type": "esriFieldTypeString", "alias": "label", "length": 64}]
polys, points = [], []
for f in src["features"]:
    g = f["geometry"]
    attrs = {k: f["properties"][k] for k in ("name", "kind", "rank", "height", "label")}
    if g["type"] == "Polygon":
        polys.append({"attributes": attrs, "geometry": {"rings": esri_rings([g["coordinates"]])}})
    elif g["type"] == "MultiPolygon":
        polys.append({"attributes": attrs, "geometry": {"rings": esri_rings(g["coordinates"])}})
    elif g["type"] == "Point":
        x, y = g["coordinates"]
        points.append({"attributes": attrs, "geometry": {"x": x, "y": y}})
sr = {"wkid": 4326, "latestWkid": 4326}
with open(sys.argv[2], "w", encoding="utf-8") as out:
    json.dump({"displayFieldName": "name", "geometryType": "esriGeometryPolygon", "spatialReference": sr,
               "fields": fields, "features": polys}, out, ensure_ascii=False, indent=1)
with open(sys.argv[2].replace(".esrijson", "_points.esrijson"), "w", encoding="utf-8") as out:
    json.dump({"geometryType": "esriGeometryPoint", "spatialReference": sr, "fields": fields, "features": points},
              out, ensure_ascii=False)
