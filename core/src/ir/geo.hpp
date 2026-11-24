// Convertible core - Geo IR: geographic features.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "util/bytes.hpp"
#include "util/json.hpp"

namespace cvt::ir {

/// A position. Unless `Geo::crs` names another coordinate reference system,
/// this is a point on the WGS 84 ellipsoid in decimal degrees; with a
/// projected `Geo::crs`, `lon` holds the easting / x and `lat` the northing /
/// y in that system's units.
struct Coord {
  double lon = 0.0;  ///< longitude in decimal degrees, east positive, -180..180
  double lat = 0.0;  ///< latitude in decimal degrees, north positive, -90..90
  /// Elevation in metres above the ellipsoid/sea level when recorded.
  std::optional<double> ele;
  /// Timestamp of the fix in milliseconds since the Unix epoch (UTC) when
  /// recorded (GPX track points, ...).
  std::optional<std::int64_t> time_ms;

  // -- additions (all optional; absent = not recorded) -----------------------

  /// Linear-referencing measure (the "M" of WKT/WKB/shapefile geometries).
  std::optional<double> m;
  /// Heart rate in beats per minute.
  std::optional<double> heart_rate;
  /// Cadence in revolutions (or steps) per minute.
  std::optional<double> cadence;
  /// Power in watts.
  std::optional<double> power;
  /// Speed over ground in metres per second.
  std::optional<double> speed;
  /// Temperature in degrees Celsius.
  std::optional<double> temperature;
  /// Further per-point values (FIT developer fields, unknown GPX extensions,
  /// extra CSV columns), as (name, value) pairs in source order.
  std::vector<std::pair<std::string, double>> extra;

  /// True when any sensor value (heart rate, cadence, power, speed,
  /// temperature or an `extra` value) is present.
  [[nodiscard]] bool has_sensors() const noexcept {
    return heart_rate || cadence || power || speed || temperature || !extra.empty();
  }

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
/// For a track (`FeatureRole::Track`) each line is one track segment.
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

/// Readers must not nest geometry collections deeper than this.
inline constexpr int max_geometry_depth = 16;

/// Any geometry (the GeoJSON model).
struct Geometry {
  using Shape = std::variant<Point, LineString, Polygon, MultiPoint, MultiLineString, MultiPolygon, GeometryCollection>;
  Shape shape;
};

/// What a feature stands for in GPS formats (GPX, TCX, FIT, ...).
enum class FeatureRole : std::uint8_t {
  Generic,   ///< any feature (GeoJSON, shapefile, KML placemark, ...)
  Waypoint,  ///< a single named position (Point)
  Route,     ///< a planned path (LineString)
  Track,     ///< a recorded path (MultiLineString, one line per segment)
};

/// Presentation of a feature (KML styles, simplestyle properties, ...).
/// Colours are CSS hex strings "#rrggbb"; empty / 0 means "not specified".
struct Style {
  std::string stroke;          ///< line / outline colour
  double stroke_opacity = 1.0; ///< 0..1
  double stroke_width = 0.0;   ///< in pixels; 0 = unspecified
  std::string fill;            ///< polygon fill colour
  double fill_opacity = 1.0;   ///< 0..1
  std::string marker_color;    ///< point marker colour
  std::string icon;            ///< point icon reference (URL or resource path), never fetched
  double icon_scale = 0.0;     ///< 0 = unspecified

  [[nodiscard]] bool empty() const noexcept {
    return stroke.empty() && stroke_width == 0.0 && fill.empty() && marker_color.empty() && icon.empty() &&
           icon_scale == 0.0 && stroke_opacity == 1.0 && fill_opacity == 1.0;
  }
  friend bool operator==(const Style&, const Style&) = default;
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

  // -- additions -------------------------------------------------------------

  /// Feature identifier (GeoJSON `id`, KML `id`, OSM id, ...): a string or a
  /// number; null when the source has none.
  Json id;
  /// Waypoint / route / track for GPS formats; Generic otherwise.
  FeatureRole role = FeatureRole::Generic;
  /// Free-text description (GPX <desc>, KML <description>).
  std::string description;
  /// The group the feature belongs to: KML folder path ("Trips/2024"),
  /// shapefile layer, ...; segments separated by '/'. Empty = top level.
  std::string folder;
  /// Presentation hints.
  Style style;
  /// Members that are neither geometry nor properties (GeoJSON foreign
  /// members), kept for round trips. Always an object.
  Json extra = Json::object();
};

/// A file carried along with the features (KMZ icons and overlays). Kept as
/// bytes and never interpreted.
struct GeoResource {
  std::string path;  ///< relative path inside the container, '/' separated
  Bytes data;
};

/// A collection of features (a GPX file, a KML document, a GeoJSON
/// FeatureCollection).
struct Geo {
  std::vector<Feature> features;
  /// Name of the whole collection; may be empty.
  std::string name;

  // -- additions -------------------------------------------------------------

  /// Coordinate reference system of every coordinate: empty for WGS 84
  /// longitude/latitude (EPSG:4326 axis order lon, lat, i.e. OGC:CRS84),
  /// otherwise an identifier such as "EPSG:3857" or "EPSG:32633", or - when
  /// the source CRS is not one the engine knows - its WKT text. Readers
  /// convert to WGS 84 whenever they can.
  std::string crs;
  /// Description of the whole collection.
  std::string description;
  /// Document-level metadata that may identify a person or device: creator
  /// application, author, e-mail, device serial, file creation time,
  /// keywords, links... Writers drop it when `strip_metadata` is on.
  Json metadata = Json::object();
  /// Foreign members of the top-level object (GeoJSON), kept for round trips.
  Json extra = Json::object();
  /// Embedded files (KMZ), kept as bytes.
  std::vector<GeoResource> resources;
};

}  // namespace cvt::ir
