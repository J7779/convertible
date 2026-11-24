// Convertible core - Geo IR: geographic features.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "util/json.hpp"

namespace cvt::ir {

/// A position on the WGS 84 ellipsoid.
struct Coord {
  double lon = 0.0;  ///< longitude in decimal degrees, east positive, -180..180
  double lat = 0.0;  ///< latitude in decimal degrees, north positive, -90..90
  /// Elevation in metres above the ellipsoid/sea level when recorded.
  std::optional<double> ele;
  /// Timestamp of the fix in milliseconds since the Unix epoch (UTC) when
  /// recorded (GPX track points, ...).
  std::optional<std::int64_t> time_ms;

  friend bool operator==(const Coord&, const Coord&) = default;
};

struct Geometry;

struct Point {
  Coord coord;
};
struct LineString {
  std::vector<Coord> points;
};
/// `rings[0]` is the outer boundary, any further rings are holes. Rings are
/// closed (first point == last point).
struct Polygon {
  std::vector<std::vector<Coord>> rings;
};
struct MultiPoint {
  std::vector<Coord> points;
};
struct MultiLineString {
  std::vector<LineString> lines;
};
struct MultiPolygon {
  std::vector<Polygon> polygons;
};
/// A heterogeneous group. Readers must not nest collections deeper than 16.
struct GeometryCollection {
  std::vector<Geometry> geometries;
};

/// Any geometry (the GeoJSON model).
struct Geometry {
  using Shape = std::variant<Point, LineString, Polygon, MultiPoint, MultiLineString, MultiPolygon, GeometryCollection>;
  Shape shape;
};

/// A geometry plus attributes: a waypoint, track, route, placemark, region...
struct Feature {
  Geometry geometry;
  /// Attributes as a JSON object (GeoJSON `properties`); an empty object
  /// when there are none.
  Json properties = Json::object();
  /// Display name when the source has one (GPX <name>, KML <name>); writers
  /// fall back to `properties["name"]`.
  std::string name;
};

/// A collection of features (a GPX file, a KML document, a GeoJSON
/// FeatureCollection).
struct Geo {
  std::vector<Feature> features;
  /// Name of the whole collection; may be empty.
  std::string name;
};

}  // namespace cvt::ir
