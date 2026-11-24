// Convertible core - geo family.
// Geographic vector data, GPS tracks and fitness files: GeoJSON (+ sequences),
// TopoJSON, Esri JSON, KML/KMZ, GPX, TCX, FIT, IGC, NMEA, WKT/WKB, GML,
// GeoRSS, OpenStreetMap XML/PBF, CSV with coordinates, zipped shapefiles,
// MapInfo MIF/TAB, FlatGeobuf, Geobuf, encoded polylines, geohashes and
// Plus Codes, plus bridges from Geo to Table, Data, Document and Image.
// Coordinate operations live in geo_ops.hpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/geo/geo.hpp"

namespace cvt {

void register_geo_family(Registry& registry) {
  geo::register_geojson(registry);
  geo::register_topojson(registry);
  geo::register_esrijson(registry);
  geo::register_takeout(registry);
  geo::register_kml(registry);
  geo::register_gpx(registry);
  geo::register_tcx(registry);
  geo::register_fit(registry);
  geo::register_igc(registry);
  geo::register_nmea(registry);
  geo::register_wkt(registry);
  geo::register_gml(registry);
  geo::register_osm(registry);
  geo::register_geocsv(registry);
  geo::register_shapefile(registry);
  geo::register_mapinfo(registry);
  geo::register_flatgeobuf(registry);
  geo::register_geobuf(registry);
  geo::register_polyline(registry);
  geo::register_geocodes(registry);
  geo::register_bridges(registry);
}

}  // namespace cvt
