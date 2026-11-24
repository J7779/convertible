// Convertible core - geo family: coordinate operations.
//
// Everything here is written in-house against published formulas and works
// on the Geo IR in place:
//
//   Coordinate reference systems
//     parse_crs()      recognises EPSG codes (in every common spelling: "EPSG:3857",
//                      "urn:ogc:def:crs:EPSG::4326", "http://www.opengis.net/def/crs/EPSG/0/32633",
//                      "OGC:CRS84"), and WKT / ESRI .prj text of the supported systems
//     Supported:       WGS 84 geographic (EPSG:4326 / CRS84; ETRS89 4258 and NAD83 4269
//                      are treated as WGS 84, < 1 m apart), Web Mercator (EPSG:3857,
//                      aliases 900913, 3785, 102100, 102113), UTM on WGS 84 (EPSG:326zz north,
//                      327zz south), on NAD83 (269zz, zones 1-23) and on ETRS89 (258zz,
//                      zones 28-38), British National Grid (EPSG:27700: OSGB36 / Airy 1830
//                      transverse Mercator reached by a 7-parameter Helmert transformation,
//                      accurate to a few metres like every Helmert-only OSGB conversion).
//     UTM uses Krueger's series to 6th order (sub-millimetre inside a zone and
//     still millimetre-accurate several degrees outside it).
//   Measurement
//     geodesic_distance()   Vincenty's inverse formula on WGS 84 (Karney-free; falls
//                           back to a spherical great circle for near-antipodal pairs)
//     length_m(), area_m2() geodesic length; polygon area on the authalic sphere
//                           (within 0.1 % of the ellipsoidal area for regions < 1000 km)
//     bounding_box()        per geometry / collection, antimeridian unaware
//     track_stats()         distance, elevation gain/loss, duration, moving average speed
//   Editing
//     simplify()            Douglas-Peucker or Visvalingam-Whyatt with a tolerance in metres;
//                           rings keep >= 4 points and are never made self-intersecting,
//                           lines keep their end points
//     round_coordinates()   round to N decimals
//     orient_rings()        outer rings counter-clockwise and holes clockwise (RFC 7946)
//                           or the reverse (Esri, shapefile)
//     split_antimeridian()  cut lines and polygons that cross +-180 degrees (RFC 7946 3.1.9)
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "ir/geo.hpp"

namespace cvt::geo {

// --------------------------------------------------------------------------- CRS

enum class CrsKind : std::uint8_t {
  Geographic,           ///< longitude / latitude in degrees on WGS 84 (or a datum treated as such)
  WebMercator,          ///< EPSG:3857 spherical Mercator, metres
  Utm,                  ///< Universal Transverse Mercator, metres
  BritishNationalGrid,  ///< EPSG:27700, metres
};

enum class Datum : std::uint8_t { Wgs84, Nad83, Etrs89 };

struct Crs {
  CrsKind kind = CrsKind::Geographic;
  Datum datum = Datum::Wgs84;
  int zone = 0;       ///< UTM zone 1..60
  bool north = true;  ///< UTM hemisphere
  int epsg = 4326;    ///< EPSG code

  friend bool operator==(const Crs&, const Crs&) = default;
};

/// Recognises a CRS from an identifier or WKT text; nullopt when unsupported.
/// Empty text is WGS 84.
[[nodiscard]] std::optional<Crs> parse_crs(std::string_view text);
/// The EPSG code of an identifier such as "EPSG:4326" or a URN/URL; nullopt
/// when the text is not an EPSG reference (whether supported or not).
[[nodiscard]] std::optional<int> epsg_code(std::string_view text);
/// "EPSG:<code>".
[[nodiscard]] std::string crs_id(const Crs& crs);
/// ESRI-flavoured WKT 1 (as written to shapefile .prj files).
[[nodiscard]] std::string crs_esri_wkt(const Crs& crs);
/// OGC WKT 1 with AUTHORITY tags.
[[nodiscard]] std::string crs_ogc_wkt(const Crs& crs);
/// True for the geographic WGS 84 CRS (empty text included).
[[nodiscard]] bool is_wgs84(std::string_view crs_text);

/// The UTM zone (1..60) and hemisphere for a WGS 84 position, honouring the
/// Norway and Svalbard exceptions.
[[nodiscard]] Crs utm_crs_for(double lon, double lat, Datum datum = Datum::Wgs84);

/// Forward / inverse projection of one position (x = easting, y = northing).
struct XY {
  double x = 0.0;
  double y = 0.0;
};
[[nodiscard]] XY project(const Crs& crs, double lon, double lat);
[[nodiscard]] XY unproject(const Crs& crs, double x, double y);  ///< returns (lon, lat)

/// Converts every coordinate of `geo` from its CRS to `target` and sets
/// `geo.crs` (empty for WGS 84). Fails with Status::InvalidArgument when
/// either CRS is not supported.
[[nodiscard]] Result<void> reproject(ir::Geo& geo, const Crs& target);

// --------------------------------------------------------------------------- measurement

/// WGS 84 ellipsoid.
inline constexpr double wgs84_a = 6378137.0;
inline constexpr double wgs84_f = 1.0 / 298.257223563;

/// Distance in metres along the WGS 84 ellipsoid.
[[nodiscard]] double geodesic_distance(double lon1, double lat1, double lon2, double lat2);
[[nodiscard]] inline double geodesic_distance(const ir::Coord& a, const ir::Coord& b) {
  return geodesic_distance(a.lon, a.lat, b.lon, b.lat);
}

/// Total geodesic length of all lines and ring perimeters, in metres.
[[nodiscard]] double length_m(const ir::Geometry& geometry);
/// Area of all polygons (holes subtracted), in square metres.
[[nodiscard]] double area_m2(const ir::Geometry& geometry);
/// Area of one closed ring in square metres; positive when counter-clockwise.
[[nodiscard]] double ring_area_m2(const std::vector<ir::Coord>& ring);

struct BoundingBox {
  double min_lon = 0.0, min_lat = 0.0, max_lon = 0.0, max_lat = 0.0;
  std::optional<double> min_ele, max_ele;
  bool empty = true;

  void add(const ir::Coord& c);
  void add(const BoundingBox& other);
};
[[nodiscard]] BoundingBox bounding_box(const ir::Geometry& geometry);
[[nodiscard]] BoundingBox bounding_box(const ir::Geo& geo);

/// Summary of a recorded path.
struct TrackStats {
  std::size_t points = 0;
  std::size_t segments = 0;
  double distance_m = 0.0;  ///< sum over segments (gaps between segments not counted)
  double elevation_gain_m = 0.0;
  double elevation_loss_m = 0.0;
  std::optional<double> min_ele, max_ele;
  std::optional<std::int64_t> start_ms, end_ms;
  std::optional<double> duration_s;     ///< end - start
  std::optional<double> moving_time_s;  ///< time spent in segments
  std::optional<double> average_speed_mps;  ///< distance / moving time
  std::optional<double> max_speed_mps;
  std::optional<double> average_heart_rate, max_heart_rate;
  std::optional<double> average_cadence, average_power;
};
/// Statistics of the lines of a geometry (LineString, MultiLineString, or
/// the lines inside a collection). Elevation gain ignores changes smaller
/// than `elevation_threshold_m` between retained samples (hysteresis), which
/// suppresses GPS noise; 0 counts every change.
[[nodiscard]] TrackStats track_stats(const ir::Geometry& geometry, double elevation_threshold_m = 0.0);

// --------------------------------------------------------------------------- editing

enum class SimplifyMethod : std::uint8_t { DouglasPeucker, Visvalingam };

/// Simplifies every line and ring of `geo` with the given tolerance in
/// metres (Visvalingam: the square root of the effective area, so the same
/// number gives comparable results). Points are never removed from
/// MultiPoints. Rings keep at least four points (first == last) and a
/// simplified ring that would cross itself is retried with a smaller
/// tolerance and kept unchanged as a last resort. Works on WGS 84 and on
/// projected coordinates (`projected` = true: tolerance in CRS units).
void simplify(ir::Geo& geo, double tolerance_m, SimplifyMethod method, bool projected = false);
/// The same for one sequence (exposed for tests): returns the kept indices.
[[nodiscard]] std::vector<std::size_t> simplify_indices(const std::vector<ir::Coord>& points, double tolerance,
                                                        SimplifyMethod method, bool projected, bool ring);

/// Rounds longitudes / latitudes (and elevations to 3 decimals beyond
/// `decimals` - 4, at least millimetres) to `decimals` decimal places.
void round_coordinates(ir::Geo& geo, int decimals);

/// Orientation of rings: `outer_ccw` = true gives the RFC 7946 rule (outer
/// counter-clockwise, holes clockwise); false the Esri / shapefile rule.
void orient_rings(ir::Geometry& geometry, bool outer_ccw);
/// Signed planar area (shoelace, in squared coordinate units); positive when
/// the ring runs counter-clockwise.
[[nodiscard]] double signed_area(const std::vector<ir::Coord>& ring);

/// Builds polygons from loose rings (shapefile, Esri JSON, MapInfo): rings
/// turning the "outer" way (`outer_clockwise`: clockwise, as Esri and
/// shapefiles store them) start polygons, the others become holes of the
/// smallest outer ring containing them (or polygons of their own when none
/// does). Rings are closed if needed; the result (Polygon when there is
/// one, else MultiPolygon) follows RFC 7946 orientation.
[[nodiscard]] ir::Geometry assemble_rings(std::vector<std::vector<ir::Coord>> rings, bool outer_clockwise);

/// True when two segments of the ring (other than neighbours) cross or touch.
[[nodiscard]] bool ring_self_intersects(const std::vector<ir::Coord>& ring);

/// Splits lines and polygons that cross the antimeridian into parts on
/// either side (LineString -> MultiLineString, Polygon -> MultiPolygon).
/// Geometries that do not cross are returned unchanged. A crossing is a step
/// of more than 180 degrees of longitude between consecutive points.
[[nodiscard]] ir::Geometry split_antimeridian(const ir::Geometry& geometry);

/// Joins every track (and every segment of every track) into one track with
/// one segment, ordered as found.
void merge_tracks(ir::Geo& geo);
/// Turns every segment of every multi-segment track into its own track.
void split_tracks(ir::Geo& geo);

}  // namespace cvt::geo
