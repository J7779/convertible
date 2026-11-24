// Convertible core - geo family: internal declarations shared by the format
// files (registration helpers, time and number formatting, sniffing).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "ir/geo.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/xml.hpp"

namespace cvt::geo {

// --------------------------------------------------------------------------- registration

// One function per format group; each lives in its own .cpp file.
void register_geojson(Registry& registry);     // geojson, geojsonseq        (geojson.cpp)
void register_topojson(Registry& registry);    // topojson                   (topojson.cpp)
void register_esrijson(Registry& registry);    // esrijson                   (esrijson.cpp)
void register_takeout(Registry& registry);     // google location history    (takeout.cpp)
void register_kml(Registry& registry);         // kml, kmz                   (kml.cpp)
void register_gpx(Registry& registry);         // gpx                        (gpx.cpp)
void register_tcx(Registry& registry);         // tcx                        (tcx.cpp)
void register_fit(Registry& registry);         // fit                        (fit.cpp)
void register_igc(Registry& registry);         // igc                        (igc.cpp)
void register_nmea(Registry& registry);        // nmea                       (nmea.cpp)
void register_wkt(Registry& registry);         // wkt, wkb, wkb-hex          (wkt.cpp, wkb.cpp)
void register_gml(Registry& registry);         // gml, georss                (gml.cpp)
void register_osm(Registry& registry);         // osm, osm.pbf               (osm.cpp, osm_pbf.cpp)
void register_geocsv(Registry& registry);      // geocsv                     (geocsv.cpp)
void register_shapefile(Registry& registry);   // shp.zip, shp               (shapefile.cpp, dbf.cpp)
void register_mapinfo(Registry& registry);     // mif, tab                   (mapinfo.cpp)
void register_flatgeobuf(Registry& registry);  // fgb                        (flatgeobuf.cpp)
void register_geobuf(Registry& registry);      // geobuf                     (geobuf.cpp)
void register_polyline(Registry& registry);    // polyline, polyline6        (polyline.cpp)
void register_geocodes(Registry& registry);    // geohash, pluscode          (geocodes.cpp)
void register_bridges(Registry& registry);     // Geo -> Table/Data/Document/Image (bridges.cpp, render.cpp)

/// Reads a whole file into the Geo IR.
using GeoReadFn = std::function<Result<ir::Geo>(ReadContext& ctx)>;
/// Writes a Geo value that `prepare_for_write` has already processed.
using GeoWriteFn = std::function<Result<Bytes>(const ir::Geo& geo, WriteContext& ctx)>;

/// What a geo format can hold; decides which common options its writer offers.
struct GeoWriterTraits {
  /// The format can declare a coordinate reference system other than WGS 84
  /// (shapefile .prj, GML srsName, EWKT SRID, ...): `target_crs` is offered.
  bool crs_capable = false;
  /// The format records per-point time (GPX, KML gx:Track, ...).
  bool has_time = false;
  /// The format records per-point sensor data (heart rate, cadence, ...).
  bool has_sensors = false;
  /// The format distinguishes tracks and their segments.
  bool has_tracks = false;
};

/// Everything `add_geo_codec` needs for one format.
struct GeoCodecSpec {
  std::string format_id;
  GeoReadFn read;    ///< may be empty (write-only format)
  GeoWriteFn write;  ///< may be empty (read-only format)
  GeoWriterTraits traits;
  /// Format-specific options, appended to the common ones.
  std::vector<OptionSpec> read_options;
  std::vector<OptionSpec> write_options;
  int read_cost = 10;
  int write_cost = 10;
  /// True when the writer drops information by design (e.g. points only).
  bool lossy_write = false;
};

/// Registers a Geo reader and/or writer. The reader gains the common read
/// options (`source_crs`) and its result is normalised (reprojected to WGS 84
/// when the CRS is known; otherwise kept with a warning). The writer gains
/// the common write options and receives the value after `prepare_for_write`.
void add_geo_codec(Registry& registry, GeoCodecSpec spec);

/// Common options of every geo reader.
[[nodiscard]] std::vector<OptionSpec> common_read_options();
/// Common options of every geo writer (and the Geo bridges).
[[nodiscard]] std::vector<OptionSpec> common_write_options(const GeoWriterTraits& traits);

/// Applies the read-side normalisation: honours `source_crs`, then converts
/// coordinates of a known CRS to WGS 84 (warning when the CRS is unknown).
[[nodiscard]] Result<void> normalize_after_read(ir::Geo& geo, const Context& ctx);

/// Applies the common write options to a copy of `geo`: target CRS,
/// track merge/split, dropping times / sensors, simplification (explicit
/// `simplify_tolerance` or derived from `quality`/`preset`), coordinate
/// precision and `strip_metadata`.
[[nodiscard]] Result<ir::Geo> prepare_for_write(const ir::Geo& geo, const Context& ctx,
                                                const GeoWriterTraits& traits);

/// The number of decimals a writer should use for coordinates: the
/// `precision` option when set, a value derived from `quality` below 90, or
/// -1 (shortest round-trip representation).
[[nodiscard]] int resolved_precision(const Options& options, bool projected);

// --------------------------------------------------------------------------- numbers and time

/// Locale-independent number text. `decimals` < 0 gives the shortest text
/// that reads back as the same double; otherwise at most that many decimals
/// with trailing zeros removed ("12.5", "3", "-0.0001").
[[nodiscard]] std::string format_number(double value, int decimals = -1);

/// ISO 8601 / RFC 3339 date-time ("2024-03-10T08:00:00Z", fractional
/// seconds, "+02:00" offsets, a date alone, a space instead of 'T'), as
/// milliseconds since the Unix epoch (UTC). Nullopt when malformed.
[[nodiscard]] std::optional<std::int64_t> parse_time(std::string_view text);
/// "2024-03-10T08:00:00Z", with ".123" milliseconds only when non-zero
/// (or always, with `always_millis`).
[[nodiscard]] std::string format_time(std::int64_t unix_ms, bool always_millis = false);
/// Days since 1970-01-01 for a proleptic Gregorian date, and back.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept;
struct CivilDate {
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};
[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept;

/// Parses a decimal number with surrounding white space allowed.
[[nodiscard]] std::optional<double> parse_number(std::string_view text);

// --------------------------------------------------------------------------- geometry helpers

/// "Point", "LineString", ... (GeoJSON spelling).
[[nodiscard]] std::string_view geometry_type_name(const ir::Geometry& geometry) noexcept;

/// Calls `f(ir::Coord&)` for every coordinate (collections included).
template <class F>
void for_each_coord(ir::Geometry& geometry, F&& f, int depth = 0) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::Point>) {
          f(shape.coord);
        } else if constexpr (std::is_same_v<T, ir::LineString> || std::is_same_v<T, ir::MultiPoint>) {
          for (ir::Coord& c : shape.points) f(c);
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          for (auto& ring : shape.rings) for (ir::Coord& c : ring) f(c);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (auto& line : shape.lines) for (ir::Coord& c : line.points) f(c);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (auto& polygon : shape.polygons) for (auto& ring : polygon.rings) for (ir::Coord& c : ring) f(c);
        } else {
          for (ir::Geometry& g : shape.geometries) for_each_coord(g, f, depth + 1);
        }
      },
      geometry.shape);
}
template <class F>
void for_each_coord(const ir::Geometry& geometry, F&& f, int depth = 0) {
  for_each_coord(const_cast<ir::Geometry&>(geometry), [&](const ir::Coord& c) { f(c); }, depth);
}
template <class F>
void for_each_coord(ir::Geo& geo, F&& f) {
  for (ir::Feature& feature : geo.features) for_each_coord(feature.geometry, f);
}

/// Number of coordinates in a geometry.
[[nodiscard]] std::size_t coord_count(const ir::Geometry& geometry);

/// True for a geometry without coordinates (an empty collection, a
/// LineString without points, ...).
[[nodiscard]] bool geometry_empty(const ir::Geometry& geometry);

/// The display name of a feature: `name`, else `properties["name"]` when it
/// is a string, else "".
[[nodiscard]] std::string feature_name(const ir::Feature& feature);

/// The value as text for formats that only hold strings: strings as they
/// are, numbers in shortest form, booleans "true"/"false", null "",
/// structures as compact JSON.
[[nodiscard]] std::string json_to_text(const Json& value);

/// Parses property text back into a typed value: integers, decimals,
/// "true"/"false"; anything else stays a string. Used by readers of
/// text-only attribute formats when `typed` is requested.
[[nodiscard]] Json text_to_json(std::string_view text);

/// True when at least one coordinate of the geometry has a timestamp.
[[nodiscard]] bool has_times(const ir::Geometry& geometry);

// --------------------------------------------------------------------------- GeoJSON object model (geojson.cpp)

/// How Geo values are turned into GeoJSON objects.
struct GeoJsonStyle {
  /// RFC 7946: outer rings counter-clockwise, holes clockwise, and lines /
  /// polygons crossing the antimeridian split.
  bool rfc7946 = true;
  /// Add `bbox` members to the collection and to every feature.
  bool bbox = false;
  /// Write per-point times and sensor values as
  /// `properties.coordinateProperties` (the togeojson convention) and point
  /// times as `properties.time`.
  bool coordinate_properties = true;
  /// Write `Feature::style` as simplestyle-spec properties.
  bool simplestyle = true;
  /// Write the role of GPS features as `properties._gpxType` ("wpt", "rte", "trk").
  bool gpx_type = true;
};

/// A GeoJSON geometry object (or null, which gives an empty collection).
[[nodiscard]] Result<ir::Geometry> geometry_from_geojson(const Json& value, int depth = 0);
/// A geometry object; an empty GeometryCollection becomes null.
[[nodiscard]] Json geometry_to_geojson(const ir::Geometry& geometry);
/// One Feature object (geometry, properties, id, foreign members).
[[nodiscard]] Result<ir::Feature> feature_from_geojson(const Json& value, const Context& ctx);
[[nodiscard]] Json feature_to_geojson(const ir::Feature& feature, const GeoJsonStyle& style);
/// A FeatureCollection, a Feature or a bare geometry.
[[nodiscard]] Result<ir::Geo> geo_from_geojson(const Json& root, const Context& ctx);
/// A FeatureCollection.
[[nodiscard]] Json geo_to_geojson(const ir::Geo& geo, const GeoJsonStyle& style);

// --------------------------------------------------------------------------- detection and parsing helpers

/// The first `limit` bytes as text, without a UTF-8 BOM and leading white space.
[[nodiscard]] std::string_view text_head(ByteView data, std::size_t limit = 4096) noexcept;
/// ASCII case-sensitive search in the head of the data.
[[nodiscard]] bool head_contains(ByteView data, std::string_view needle, std::size_t limit = 4096) noexcept;
/// True when `data` is a ZIP archive with an entry whose name ends in
/// `suffix` (ASCII case-insensitive), ignoring macOS resource-fork entries.
[[nodiscard]] bool zip_has_entry_with_extension(ByteView data, std::string_view suffix);
/// True when the file name ends in `.extension` (ASCII case-insensitive).
[[nodiscard]] bool has_extension(std::string_view filename, std::string_view extension) noexcept;

/// XML parse options for geo readers (bounded depth and node count).
[[nodiscard]] util::XmlParseOptions xml_options();

/// Input as UTF-8 text without a BOM (UTF-16 inputs are transcoded).
[[nodiscard]] std::string input_text(ByteView data);

/// Guard for counts declared by a file: fails with Status::Decode when
/// `count * min_bytes_each` exceeds what is left of the input.
[[nodiscard]] Result<void> check_declared(std::uint64_t count, std::uint64_t min_bytes_each, std::uint64_t remaining,
                                          std::string_view what);

}  // namespace cvt::geo
