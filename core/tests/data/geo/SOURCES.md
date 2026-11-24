# Geo fixtures

Every fixture of the geo family is derived from one synthetic data set:
made-up features in the open Atlantic (UTM zone 26N) named after the NATO
alphabet, with typed properties (string, integer, float, boolean, Unicode
text), and a two-segment track with time, elevation, heart rate, cadence,
power and temperature. `tools/synthetic.py` writes the base files with the
Python standard library; every other file was converted from them by
independent software. The same values are available to the tests as C++ in
`core/tests/families/geo/synthetic.hpp`.

All conversions are in `tools/make_fixtures.sh`, run in a throwaway container
built from `tools/Dockerfile` (Ubuntu 24.04 packages: GDAL 3.8.4, GPSBabel
1.9.0, osmium-tool 1.16.0, pyproj 3.6.1 / PROJ, shapely 2.2.0, fiona 1.9.5,
pyshp 2.3.1; pip: fitparse 1.2.0, fit-tool 0.10.0, topojson 2.2, geobuf
2.0.1, polyline 2.0.5, openlocationcode 1.0.1, pygeohash 3.6.0; npm:
topojson-server 3.0.1, topojson-client 3.1.0, flatgeobuf 3.36.0), from this
directory:

    docker build -t cvt-geo-fixtures tools
    docker run --rm --memory 2g -e GPSBABEL_FREEZE_TIME=y -v "$PWD:/w" cvt-geo-fixtures sh /w/tools/make_fixtures.sh

`$F` below is `synthetic_features.geojson`, `$T` is `synthetic_track.gpx`.
ZIP bundles are made with `tools/zipdir.py` (fixed 1980 timestamps, no extra
fields).

## Base files (Python standard library, `python3 tools/synthetic.py .`)

| File | Content |
| --- | --- |
| `synthetic_features.geojson` | 7 features: Point x2 (Alpha Beacon, Bravo Buoy), LineString (Charlie Path), Polygon with a hole (Delta Field), MultiPolygon with one hole (Echo Islands), MultiLineString (Foxtrot Lines), MultiPoint (Golf Points); properties name / kind / rank (int) / height (float) / active (bool) / label (Unicode) |
| `synthetic_rich.geojson` | FeatureCollection with `name`, `bbox`, a foreign member, string and numeric ids, a GeometryCollection with a 3D point, a feature with null geometry and properties |
| `synthetic_track.gpx` | GPX 1.1: waypoint India Camp, route Juliet Route (3 points), track Hotel Run with segments of 6 and 5 points every 10 s from 2024-03-10T08:00:00Z, `ele`, Garmin TrackPointExtension `hr` / `cad` / `atemp`, PowerExtension watts |

## Converted files

| File | Tool | Command (see `tools/make_fixtures.sh`) |
| --- | --- | --- |
| `json/features_gdal.geojson` | GDAL 3.8.4 | `ogr2ogr -f GeoJSON -lco RFC7946=YES json/features_gdal.geojson $F` |
| `json/features_gdal.geojsonl` | GDAL | `ogr2ogr -f GeoJSONSeq json/features_gdal.geojsonl $F` |
| `json/features_gdal_rs.geojsons` | GDAL | `ogr2ogr -f GeoJSONSeq -lco RS=YES json/features_gdal_rs.geojsons $F` (RFC 8142 record separators) |
| `json/features_3857_gdal.geojson` | GDAL | `ogr2ogr -f GeoJSON -t_srs EPSG:3857 json/features_3857_gdal.geojson $F` (legacy `crs` member) |
| `json/proj_reference.txt` | pyproj 3.6.1 | `/venv/bin/python tools/proj_reference.py` - projected coordinates (Web Mercator, UTM 26N / 33S, ETRS89 UTM 32N, NAD83 UTM 15N, British National Grid with the OSGB36 Helmert parameters), WGS 84 geodesic distances, polygon areas and perimeters |
| `topojson/features_q1e5.topojson` | topojson-server 3.0.1 | `geo2topo -q 100000 features=$F` (quantised, delta-coded arcs) |
| `topojson/features_plain.topojson` | topojson-server | `geo2topo features=$F` (no transform) |
| `esrijson/features.esrijson`, `esrijson/features_points.esrijson` | Python json | `tools/esri_json.py $F esrijson/features.esrijson` (GDAL cannot write Esri JSON): polygon feature set with clockwise outer rings, point feature set |
| `geobuf/features.geobuf` | geobuf 2.0.1 (Python) | `geobuf.encode(json.load(open($F)), 7)` |
| `kml/features_ogr.kml` | GDAL | `ogr2ogr -f KML kml/features_ogr.kml $F` (Schema + SimpleData) |
| `kml/features_libkml.kml` | GDAL LIBKML | `ogr2ogr -f LIBKML kml/features_libkml.kml $F` (nested Documents) |
| `kml/features_libkml.kmz` | GDAL LIBKML | `ogr2ogr -f LIBKML kml/features_libkml.kmz $F` (doc.kml links layers/synthetic_features.kml inside the archive) |
| `kml/track_gpsbabel.kml` | GPSBabel 1.9.0 | `gpsbabel -i gpx -f $T -o kml,points=1,track=1 -F kml/track_gpsbabel.kml` (gx:Track with heart rate / cadence / temperature arrays, folders) |
| `gpx/track_gpsbabel_10.gpx` | GPSBabel | `gpsbabel -i gpx -f $T -o gpx,gpxver=1.0 -F gpx/track_gpsbabel_10.gpx` |
| `gpx/track_gpsbabel_11.gpx` | GPSBabel | `gpsbabel -i gpx -f $T -o gpx,garminextensions -F gpx/track_gpsbabel_11.gpx` |
| `gpx/points_ogr.gpx` | GDAL | `ogr2ogr -f GPX -dsco GPX_USE_EXTENSIONS=YES gpx/points_ogr.gpx $F -nlt POINT -where "kind IN ('beacon','buoy')"` |
| `tcx/activity_gpsbabel.tcx` | GPSBabel | `gpsbabel -i gpx -f $T -o gtrnctr,course=0,sport=Running -F tcx/activity_gpsbabel.tcx` |
| `tcx/course_gpsbabel.tcx` | GPSBabel | `gpsbabel -i gpx -f $T -o gtrnctr,course=1 -F tcx/course_gpsbabel.tcx` |
| `fit/activity_fittool.fit` | fit-tool 0.10.0 | `/venv/bin/python tools/fit_activity.py fit/activity_fittool.fit` - activity: file_id (manufacturer development, no serial), timer events, 11 records (position, altitude, heart rate, cadence, power, temperature), 2 laps, session, activity |
| `fit/activity_fittool.records.json` | fitparse 1.2.0 | the record messages of the file above as decoded by fitparse (reference values) |
| `fit/course_gpsbabel.fit` | GPSBabel | `gpsbabel -i gpx -f $T -o garmin_fit -F fit/course_gpsbabel.fit` |
| `igc/track_gpsbabel.igc` | GPSBabel | `gpsbabel -i gpx -f $T -o igc -F igc/track_gpsbabel.igc` |
| `nmea/track_gpsbabel.nmea` | GPSBabel | `gpsbabel -i gpx -f $T -o nmea -F nmea/track_gpsbabel.nmea` (GPWPL, GPRMC, GPGGA) |
| `wkt/features.wkt`, `wkt/features_z.wkt`, `wkt/collection.wkt`, `wkt/features_le.wkb`, `wkt/features_be.wkb`, `wkt/features_z_iso.wkb`, `wkt/features_ewkb.hex` | shapely 2.2.0 (GEOS) | `/venv/bin/python tools/wkt_fixtures.py`: WKT, 3D WKT (z = (lon + 30) * 1000), a nested collection, concatenated WKB in both byte orders, ISO WKB Z, EWKB hex with SRID 4326 |
| `gml/features_gml2.gml`, `gml/features_gml3.gml`, `gml/features_gml32.gml` | GDAL | `ogr2ogr -f GML -dsco FORMAT=GML2 / GML3 / GML3.2 ...` (GML 3 uses urn:ogc:def:crs:EPSG::4326, latitude first) |
| `gml/features_gml3_utm.gml` | GDAL | `ogr2ogr -f GML -dsco FORMAT=GML3 -t_srs EPSG:32626 gml/features_gml3_utm.gml $F` |
| `gml/features_georss_simple.xml`, `gml/features_georss_gml.xml` | GDAL | `ogr2ogr -f GeoRSS [-dsco GEOM_DIALECT=GML] ... -dialect sqlite -sql "SELECT name AS title, label AS description, geometry ..."` (GDAL writes the GML dialect longitude first) |
| `csv/features_wkt.csv` | GDAL | `ogr2ogr -f CSV -lco GEOMETRY=AS_WKT csv/features_wkt.csv $F` |
| `csv/points_xy.csv` | GDAL | `ogr2ogr -f CSV -lco GEOMETRY=AS_XY csv/points_xy.csv $F -where "kind IN ('beacon','buoy')"` |
| `csv/track_points_xy.csv` | GDAL | `ogr2ogr -f CSV -lco GEOMETRY=AS_XY -select ele,time csv/track_points_xy.csv $T track_points` (a GPS log with times) |
| `osm/sample.osm`, `osm/sample.osm.pbf` | osmium-tool 1.16.0 | `osmium cat tools/sample.opl -o osm/sample.osm` / `-o osm/sample.osm.pbf` (dense nodes, zlib blobs): a tagged node, a path, a closed building way, a multipolygon relation whose outer ring is split over two ways, with one inner way |
| `shp/features_ogr.shp.zip` | GDAL | `ogr2ogr -f "ESRI Shapefile" -lco ENCODING=UTF-8` into points (POINT), lines (MULTILINESTRING) and polygons (MULTIPOLYGON) layers, zipped |
| `shp/polygons_utm.shp.zip` | GDAL | as above with `-t_srs EPSG:32626` (polygons, ESRI .prj without authority) |
| `shp/points_latin1.shp.zip` | GDAL | `-lco ENCODING=ISO-8859-1` with an SQLite point-on-surface query: two points, Latin-1 attributes (`.cpg` says ISO-8859-1; GDAL wrote "?" for the Omega it could not encode) |
| `shp/zm_multipatch_pyshp.shp.zip` | pyshp 2.3.1 | `/venv/bin/python tools/pyshp_fixtures.py <dir>`: PointZ (z 12.5, m 1.5), PolyLineZ (z 10 * i, m = i), PolygonM with a hole, MultiPatch (triangle strip + outer ring) |
| `shp/points_alone.shp` | GDAL | the `points.shp` of `features_ogr.shp.zip` on its own |
| `mapinfo/features_mif.zip`, `mapinfo/features_alone.mif` | GDAL | `ogr2ogr -f "MapInfo File" -dsco FORMAT=MIF ... -nlt GEOMETRY` (MultiPoint written as two Points), zipped with its .mid / the .mif alone |
| `mapinfo/features_tab.zip` | GDAL | `ogr2ogr -f "MapInfo File" ... -nlt GEOMETRY` (native TAB/MAP/DAT/ID, used to check that it is recognised) |
| `fgb/features_indexed.fgb` | GDAL | `ogr2ogr -f FlatGeobuf fgb/features_indexed.fgb $F` (packed Hilbert R-tree; features in Hilbert order) |
| `fgb/features_noindex.fgb` | GDAL | `ogr2ogr -f FlatGeobuf -lco SPATIAL_INDEX=NO fgb/features_noindex.fgb $F` |
| `polyline/lines.polyline`, `polyline/lines.polyline6` | polyline 2.0.5 | `/venv/bin/python tools/code_fixtures.py`: Charlie Path and the first track segment at precision 5 / 6 |
| `codes/points.geohash`, `codes/points_short.geohash` | pygeohash 3.6.0 | same script: three points at 11 and 6 characters |
| `codes/points.pluscode` | openlocationcode 1.0.1 | same script: three points, 11-digit codes |
| `takeout/records.json`, `takeout/semantic.json`, `takeout/timeline.json` | Python json | `python3 tools/takeout.py takeout`: the track in the three location-history layouts Google exports (made-up values; these exports cannot be produced otherwise) |

Every file was checked with `LC_ALL=C grep -aiE "<login>|/Users|/home/|\.local"` (no
matches). GDAL stamps the creation date into .dbf / .dat headers and LIBKML
into KMZ entry times; GPSBabel writes its name in GPX `creator` and the IGC
`L` record. Nothing else identifies a machine or a person.

## Interoperability of Convertible's output

Written with the `convertible` CLI from `synthetic_features.geojson` and
`synthetic_track.gpx` and read back in the same container:

| Output | Checked with | Result |
| --- | --- | --- |
| kml, kmz | `ogrinfo` (KML, LIBKML) | 7 features |
| gpx (features, track) | `ogrinfo`, `gpsbabel -i gpx` | waypoints / routes / tracks as expected; time, elevation, hr, cad, atemp read back |
| gml (3.2) | `ogrinfo` | 7 features, latitude-first axis order understood |
| fgb | `ogrinfo` | 7 features, Unicode attributes intact |
| geojsonseq, topojson | `ogrinfo`, topojson-client `feature()` | 7 features |
| esrijson | `ogrinfo ESRIJSON:` | 7 features (mixed geometry types are reported as the first type, with a warning on writing) |
| shp.zip (and with `target_crs=EPSG:32626`) | `ogrinfo /vsizip/` | 4 layers by geometry type, typed fields, UTF-8 text; the .prj is recognised as EPSG:32626 |
| mapinfo.zip | `ogrinfo /vsizip/.../layer.mif` | 7 features |
| osm | `ogrinfo`, `osmium fileinfo` | 32 nodes, 8 ways, 2 relations (positive ids: GDAL rejects negative ones) |
| geocsv | `ogrinfo -oo GEOM_POSSIBLE_NAMES=WKT` | 7 features |
| wkt | shapely `wkt.loads` | 7 valid geometries |
| geobuf | python `geobuf.decode` | 7 features, exact coordinates and properties |
| fit | fitparse (CRC checked), `gpsbabel -i garmin_fit` | 11 records, 2 laps; hr, cad, power, temperature |
| tcx | `gpsbabel -i gtrnctr` | 11 points with time, altitude, hr, cad, power |
| igc, nmea | `gpsbabel -i igc` / `-i nmea` | 11 fixes with time and altitude |
