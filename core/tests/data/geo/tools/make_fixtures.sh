#!/bin/sh
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Regenerates the geo fixtures from the synthetic base files with
# independent tools. Runs inside the container built from tools/Dockerfile:
#
#   docker run --rm --memory 2g -e GPSBABEL_FREEZE_TIME=y -v "$PWD:/w" cvt-geo-fixtures sh /w/tools/make_fixtures.sh
#
# with the working directory set to core/tests/data/geo (or a copy of it).
set -u
cd /w
PY=/venv/bin/python
F=synthetic_features.geojson
T=synthetic_track.gpx
mkdir -p json kml gpx tcx fit igc nmea wkt gml osm csv shp mapinfo fgb geobuf polyline codes topojson esrijson takeout
export GPSBABEL_FREEZE_TIME=y
export PROJ_DATA=/usr/share/proj

# ---- GeoJSON family
ogr2ogr -f GeoJSON -lco RFC7946=YES json/features_gdal.geojson $F
ogr2ogr -f GeoJSONSeq json/features_gdal.geojsonl $F
ogr2ogr -f GeoJSONSeq -lco RS=YES json/features_gdal_rs.geojsons $F
ogr2ogr -f GeoJSON -t_srs EPSG:3857 json/features_3857_gdal.geojson $F
node /npm/node_modules/topojson-server/bin/geo2topo -q 100000 features=$F > topojson/features_q1e5.topojson
node /npm/node_modules/topojson-server/bin/geo2topo features=$F > topojson/features_plain.topojson
$PY tools/esri_json.py $F esrijson/features.esrijson
$PY -c "import geobuf,json;open('geobuf/features.geobuf','wb').write(geobuf.encode(json.load(open('$F')),7))"

# ---- KML / KMZ
ogr2ogr -f KML kml/features_ogr.kml $F
ogr2ogr -f LIBKML kml/features_libkml.kml $F
ogr2ogr -f LIBKML kml/features_libkml.kmz $F
gpsbabel -i gpx -f $T -o kml,points=1,track=1 -F kml/track_gpsbabel.kml

# ---- GPX / TCX / FIT / IGC / NMEA
gpsbabel -i gpx -f $T -o gpx,gpxver=1.0 -F gpx/track_gpsbabel_10.gpx
gpsbabel -i gpx -f $T -o gpx,garminextensions -F gpx/track_gpsbabel_11.gpx
ogr2ogr -f GPX -dsco GPX_USE_EXTENSIONS=YES gpx/points_ogr.gpx $F -nlt POINT -where "kind IN ('beacon','buoy')"
gpsbabel -i gpx -f $T -o gtrnctr,course=0,sport=Running -F tcx/activity_gpsbabel.tcx
gpsbabel -i gpx -f $T -o gtrnctr,course=1 -F tcx/course_gpsbabel.tcx
$PY tools/fit_activity.py fit/activity_fittool.fit
gpsbabel -i gpx -f $T -o garmin_fit -F fit/course_gpsbabel.fit
$PY -c "
import fitparse,json
f=fitparse.FitFile('fit/activity_fittool.fit')
out=[]
for m in f.get_messages('record'):
    out.append({k.name:(k.value.isoformat() if hasattr(k.value,'isoformat') else k.value) for k in m.fields})
json.dump(out,open('fit/activity_fittool.records.json','w'),indent=0)"
gpsbabel -i gpx -f $T -o igc -F igc/track_gpsbabel.igc
gpsbabel -i gpx -f $T -o nmea -F nmea/track_gpsbabel.nmea

# ---- OGC text / binary
$PY tools/wkt_fixtures.py
ogr2ogr -f GML -dsco FORMAT=GML2 gml/features_gml2.gml $F
ogr2ogr -f GML -dsco FORMAT=GML3 gml/features_gml3.gml $F
ogr2ogr -f GML -dsco FORMAT=GML3.2 gml/features_gml32.gml $F
ogr2ogr -f GML -dsco FORMAT=GML3 -t_srs EPSG:32626 gml/features_gml3_utm.gml $F
RSS_SQL="SELECT name AS title, label AS description, geometry FROM synthetic_features WHERE kind IN ('beacon','buoy','path','field')"
ogr2ogr -f GeoRSS gml/features_georss_simple.xml $F -dialect sqlite -sql "$RSS_SQL"
ogr2ogr -f GeoRSS -dsco GEOM_DIALECT=GML gml/features_georss_gml.xml $F -dialect sqlite -sql "$RSS_SQL"
ogr2ogr -f CSV -lco GEOMETRY=AS_WKT csv/features_wkt.csv $F
ogr2ogr -f CSV -lco GEOMETRY=AS_XY csv/points_xy.csv $F -where "kind IN ('beacon','buoy')"
ogr2ogr -f CSV -lco GEOMETRY=AS_XY -select ele,time csv/track_points_xy.csv $T track_points

# ---- OpenStreetMap
osmium cat tools/sample.opl -o osm/sample.osm --overwrite
osmium cat tools/sample.opl -o osm/sample.osm.pbf --overwrite

# ---- Shapefiles (zipped), MapInfo, FlatGeobuf
rm -rf /tmp/s && mkdir -p /tmp/s/a /tmp/s/u /tmp/s/l /tmp/s/m /tmp/s/t
ogr2ogr -f "ESRI Shapefile" -lco ENCODING=UTF-8 /tmp/s/a/points.shp $F -where "kind IN ('beacon','buoy')" -nlt POINT
ogr2ogr -f "ESRI Shapefile" -lco ENCODING=UTF-8 /tmp/s/a/lines.shp $F -where "kind IN ('path','lines')" -nlt MULTILINESTRING
ogr2ogr -f "ESRI Shapefile" -lco ENCODING=UTF-8 /tmp/s/a/polygons.shp $F -where "kind IN ('field','islands')" -nlt MULTIPOLYGON
python3 tools/zipdir.py shp/features_ogr.shp.zip /tmp/s/a/*
ogr2ogr -f "ESRI Shapefile" -lco ENCODING=UTF-8 -t_srs EPSG:32626 /tmp/s/u/polygons_utm.shp $F -where "kind IN ('field','islands')" -nlt MULTIPOLYGON
python3 tools/zipdir.py shp/polygons_utm.shp.zip /tmp/s/u/*
ogr2ogr -f "ESRI Shapefile" -lco ENCODING=ISO-8859-1 /tmp/s/l/points_latin1.shp $F -where "kind IN ('beacon','buoy','field')" -nlt POINT -dialect sqlite -sql "SELECT name, kind, rank, label, ST_PointOnSurface(geometry) AS geometry FROM synthetic_features WHERE kind IN ('buoy','field')"
python3 tools/zipdir.py shp/points_latin1.shp.zip /tmp/s/l/*
$PY tools/pyshp_fixtures.py /tmp/s/t
python3 tools/zipdir.py shp/zm_multipatch_pyshp.shp.zip /tmp/s/t/*
cp /tmp/s/a/points.shp shp/points_alone.shp
ogr2ogr -f "MapInfo File" /tmp/s/m/features.tab $F -nlt GEOMETRY
ogr2ogr -f "MapInfo File" -dsco FORMAT=MIF /tmp/s/m/features.mif $F -nlt GEOMETRY
python3 tools/zipdir.py mapinfo/features_mif.zip /tmp/s/m/features.mif /tmp/s/m/features.mid
python3 tools/zipdir.py mapinfo/features_tab.zip /tmp/s/m/features.tab /tmp/s/m/features.map /tmp/s/m/features.dat /tmp/s/m/features.id
cp /tmp/s/m/features.mif mapinfo/features_alone.mif
ogr2ogr -f FlatGeobuf fgb/features_indexed.fgb $F
ogr2ogr -f FlatGeobuf -lco SPATIAL_INDEX=NO fgb/features_noindex.fgb $F

# ---- Encoded polylines, geohash, Plus Codes, Takeout
$PY tools/code_fixtures.py
$PY tools/takeout.py takeout

# ---- Reprojection references (pyproj)
$PY tools/proj_reference.py > json/proj_reference.txt
