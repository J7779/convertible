#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Writes the synthetic geographic data set every geo fixture is derived from.
# Locations are made up and lie in the open Atlantic (UTM zone 26N); names
# are NATO-alphabet placeholders. Uses the Python standard library only.
#
#   python3 synthetic.py <output-directory>
#
# Output:
#   synthetic_features.geojson   points, a line, a polygon with a hole, a
#                                multipolygon, a multilinestring and a
#                                multipoint with typed properties (RFC 7946)
#   synthetic_track.gpx          GPX 1.1: one waypoint, one route and a
#                                two-segment track with time, elevation and
#                                Garmin TrackPointExtension hr/cad/atemp, power
#   synthetic_rich.geojson       GeoJSON with ids, bbox, foreign members and a
#                                GeometryCollection (read-side features)
import json
import os
import sys

PROPS = ["name", "kind", "rank", "height", "active", "label"]


def props(name, kind, rank, height, active, label):
    return {"name": name, "kind": kind, "rank": rank, "height": height, "active": active, "label": label}


def ring(points):
    return [list(p) for p in points] + [list(points[0])]


FEATURES = [
    ({"type": "Point", "coordinates": [-29.4812345, 40.1234567]},
     props("Alpha Beacon", "beacon", 3, 12.5, True, "Ünïcødé ✓ 東京")),
    ({"type": "Point", "coordinates": [-29.45, 40.15]},
     props("Bravo Buoy", "buoy", 7, 0.25, False, "plain text")),
    ({"type": "LineString", "coordinates": [[-29.49, 40.10], [-29.48, 40.11], [-29.47, 40.105], [-29.46, 40.12]]},
     props("Charlie Path", "path", 1, 2.0, True, 'a, "quoted" label')),
    ({"type": "Polygon", "coordinates": [
        ring([(-29.50, 40.00), (-29.40, 40.00), (-29.40, 40.08), (-29.50, 40.08)]),
        ring([(-29.47, 40.02), (-29.47, 40.05), (-29.43, 40.05), (-29.43, 40.02)])]},
     props("Delta Field", "field", 2, 100.75, False, "Ωmega field")),
    ({"type": "MultiPolygon", "coordinates": [
        [ring([(-29.30, 40.00), (-29.25, 40.00), (-29.25, 40.04), (-29.30, 40.04)]),
         ring([(-29.29, 40.01), (-29.29, 40.02), (-29.28, 40.02), (-29.28, 40.01)])],
        [ring([(-29.20, 40.00), (-29.15, 40.00), (-29.175, 40.05)])]]},
     props("Echo Islands", "islands", 5, 3.5, True, "two islands")),
    ({"type": "MultiLineString", "coordinates": [
        [[-29.35, 40.20], [-29.34, 40.21]],
        [[-29.33, 40.22], [-29.32, 40.23], [-29.31, 40.22]]]},
     props("Foxtrot Lines", "lines", 4, 7.0, False, "zwei Linien")),
    ({"type": "MultiPoint", "coordinates": [[-29.10, 40.30], [-29.11, 40.31]]},
     props("Golf Points", "points", 6, 1.125, True, "deux points")),
]

# Track: two segments, 10 s apart, starting 2024-03-10T08:00:00Z and 08:05:00Z.
SEG1 = [dict(lon=-29.45 + 0.0005 * i, lat=40.20 + 0.0002 * i, ele=100.0 + 1.5 * i,
             t=f"2024-03-10T08:00:{10 * i:02d}Z", hr=120 + i, cad=80 + i, power=200 + 5 * i, temp=18)
        for i in range(6)]
SEG2 = [dict(lon=-29.44 + 0.0004 * i, lat=40.21 - 0.0001 * i, ele=110.0 - 2.0 * i,
             t=f"2024-03-10T08:05:{10 * i:02d}Z", hr=130 + i, cad=85, power=210, temp=17)
        for i in range(5)]
WAYPOINT = dict(name="India Camp", lon=-29.455, lat=40.205, ele=105.0, t="2024-03-10T07:55:00Z", desc="base camp")
ROUTE = [(-29.45, 40.20), (-29.445, 40.202), (-29.44, 40.21)]


def write_features(path):
    fc = {"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": p, "geometry": g} for g, p in FEATURES]}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(fc, f, ensure_ascii=False, indent=1)
        f.write("\n")


def write_rich(path):
    fc = {
        "type": "FeatureCollection",
        "name": "rich sample",
        "bbox": [-29.5, 40.0, -29.1, 40.31],
        "x-producer-note": {"purpose": "foreign member test", "level": 2},
        "features": [
            {"type": "Feature", "id": "alpha", "properties": FEATURES[0][1], "geometry": FEATURES[0][0],
             "x-extra": [1, 2, 3]},
            {"type": "Feature", "id": 42, "bbox": [-29.49, 40.10, -29.46, 40.12],
             "properties": FEATURES[2][1], "geometry": FEATURES[2][0]},
            {"type": "Feature", "properties": {"name": "Kilo Group", "nested": {"a": [1, 2], "b": None}},
             "geometry": {"type": "GeometryCollection", "geometries": [
                 {"type": "Point", "coordinates": [-29.0, 40.4, 15.5]},
                 {"type": "LineString", "coordinates": [[-29.0, 40.4], [-28.99, 40.41]]}]}},
            {"type": "Feature", "properties": None, "geometry": None},
        ],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(fc, f, ensure_ascii=False, indent=2)
        f.write("\n")


def write_gpx(path):
    ns = ('xmlns="http://www.topografix.com/GPX/1/1" '
          'xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v1" '
          'xmlns:pwr="http://www.garmin.com/xmlschemas/PowerExtension/v1"')
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           f'<gpx version="1.1" creator="synthetic" {ns}>',
           '  <metadata><name>Hotel Outing</name></metadata>',
           f'  <wpt lat="{WAYPOINT["lat"]}" lon="{WAYPOINT["lon"]}"><ele>{WAYPOINT["ele"]}</ele>'
           f'<time>{WAYPOINT["t"]}</time><name>{WAYPOINT["name"]}</name><desc>{WAYPOINT["desc"]}</desc></wpt>',
           '  <rte><name>Juliet Route</name>']
    for lon, lat in ROUTE:
        out.append(f'    <rtept lat="{lat}" lon="{lon}"/>')
    out.append('  </rte>')
    out.append('  <trk><name>Hotel Run</name>')
    for seg in (SEG1, SEG2):
        out.append('    <trkseg>')
        for p in seg:
            out.append(
                f'      <trkpt lat="{round(p["lat"], 7)}" lon="{round(p["lon"], 7)}"><ele>{p["ele"]}</ele>'
                f'<time>{p["t"]}</time><extensions><pwr:PowerInWatts>{p["power"]}</pwr:PowerInWatts>'
                f'<gpxtpx:TrackPointExtension><gpxtpx:atemp>{p["temp"]}</gpxtpx:atemp>'
                f'<gpxtpx:hr>{p["hr"]}</gpxtpx:hr><gpxtpx:cad>{p["cad"]}</gpxtpx:cad>'
                '</gpxtpx:TrackPointExtension></extensions></trkpt>')
        out.append('    </trkseg>')
    out.append('  </trk>')
    out.append('</gpx>')
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(target, exist_ok=True)
    write_features(os.path.join(target, "synthetic_features.geojson"))
    write_rich(os.path.join(target, "synthetic_rich.geojson"))
    write_gpx(os.path.join(target, "synthetic_track.gpx"))
