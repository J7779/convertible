// The synthetic geo data set (core/tests/data/geo/tools/synthetic.py) as C++
// values, plus comparison helpers shared by the geo family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "families/geo/geo.hpp"
#include "families/geo/geo_ops.hpp"
#include "ir/geo.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

namespace cvt::test::geo {

/// Default coordinate tolerance in degrees.
inline constexpr double coord_tolerance = 1e-7;

[[nodiscard]] inline ir::Coord ll(double lon, double lat) {
  ir::Coord c;
  c.lon = lon;
  c.lat = lat;
  return c;
}

[[nodiscard]] inline std::vector<ir::Coord> closed(std::vector<ir::Coord> ring) {
  ring.push_back(ring.front());
  return ring;
}

[[nodiscard]] inline Json props(std::string_view name, std::string_view kind, std::int64_t rank, double height,
                                bool active, std::string_view label) {
  return Json::object({{"name", name}, {"kind", kind}, {"rank", rank}, {"height", height}, {"active", active},
                       {"label", label}});
}

[[nodiscard]] inline ir::Feature feature(ir::Geometry::Shape shape, Json properties) {
  ir::Feature f;
  f.geometry.shape = std::move(shape);
  f.name = properties["name"].as_string();
  f.properties = std::move(properties);
  return f;
}

/// The seven features of synthetic_features.geojson, in file order.
[[nodiscard]] inline ir::Geo synthetic_features() {
  ir::Geo geo;
  geo.features.push_back(feature(ir::Point{ll(-29.4812345, 40.1234567)},
                                 props("Alpha Beacon", "beacon", 3, 12.5, true, "Ünïcødé ✓ 東京")));
  geo.features.push_back(feature(ir::Point{ll(-29.45, 40.15)}, props("Bravo Buoy", "buoy", 7, 0.25, false, "plain text")));
  geo.features.push_back(feature(
      ir::LineString{{ll(-29.49, 40.10), ll(-29.48, 40.11), ll(-29.47, 40.105), ll(-29.46, 40.12)}},
      props("Charlie Path", "path", 1, 2.0, true, "a, \"quoted\" label")));
  geo.features.push_back(feature(
      ir::Polygon{{closed({ll(-29.50, 40.00), ll(-29.40, 40.00), ll(-29.40, 40.08), ll(-29.50, 40.08)}),
                   closed({ll(-29.47, 40.02), ll(-29.47, 40.05), ll(-29.43, 40.05), ll(-29.43, 40.02)})}},
      props("Delta Field", "field", 2, 100.75, false, "Ωmega field")));
  geo.features.push_back(feature(
      ir::MultiPolygon{{ir::Polygon{{closed({ll(-29.30, 40.00), ll(-29.25, 40.00), ll(-29.25, 40.04), ll(-29.30, 40.04)}),
                                     closed({ll(-29.29, 40.01), ll(-29.29, 40.02), ll(-29.28, 40.02), ll(-29.28, 40.01)})}},
                        ir::Polygon{{closed({ll(-29.20, 40.00), ll(-29.15, 40.00), ll(-29.175, 40.05)})}}}},
      props("Echo Islands", "islands", 5, 3.5, true, "two islands")));
  geo.features.push_back(feature(
      ir::MultiLineString{{ir::LineString{{ll(-29.35, 40.20), ll(-29.34, 40.21)}},
                           ir::LineString{{ll(-29.33, 40.22), ll(-29.32, 40.23), ll(-29.31, 40.22)}}}},
      props("Foxtrot Lines", "lines", 4, 7.0, false, "zwei Linien")));
  geo.features.push_back(feature(ir::MultiPoint{{ll(-29.10, 40.30), ll(-29.11, 40.31)}},
                                 props("Golf Points", "points", 6, 1.125, true, "deux points")));
  return geo;
}

/// 2024-03-10T08:00:00Z in milliseconds since the epoch.
inline constexpr std::int64_t track_start_ms = 1710057600000;

[[nodiscard]] inline double round7(double v) { return std::round(v * 1e7) / 1e7; }

/// Track point `i` of segment `segment` (0 or 1) of the "Hotel Run" track.
[[nodiscard]] inline ir::Coord track_point(int segment, int i) {
  ir::Coord c;
  if (segment == 0) {
    c.lon = round7(-29.45 + 0.0005 * i);
    c.lat = round7(40.20 + 0.0002 * i);
    c.ele = 100.0 + 1.5 * i;
    c.time_ms = track_start_ms + 10'000LL * i;
    c.heart_rate = 120 + i;
    c.cadence = 80 + i;
    c.power = 200 + 5 * i;
    c.temperature = 18;
  } else {
    c.lon = round7(-29.44 + 0.0004 * i);
    c.lat = round7(40.21 - 0.0001 * i);
    c.ele = 110.0 - 2.0 * i;
    c.time_ms = track_start_ms + 300'000LL + 10'000LL * i;
    c.heart_rate = 130 + i;
    c.cadence = 85;
    c.power = 210;
    c.temperature = 17;
  }
  return c;
}
inline constexpr int track_segment_sizes[2] = {6, 5};

/// The "Hotel Run" track as a MultiLineString (segment 0: 6 points, segment 1: 5).
[[nodiscard]] inline ir::MultiLineString synthetic_track() {
  ir::MultiLineString track;
  for (int s = 0; s < 2; ++s) {
    ir::LineString line;
    for (int i = 0; i < track_segment_sizes[s]; ++i) line.points.push_back(track_point(s, i));
    track.lines.push_back(std::move(line));
  }
  return track;
}

/// Waypoint "India Camp" and route "Juliet Route" of synthetic_track.gpx.
inline constexpr double waypoint_lon = -29.455, waypoint_lat = 40.205, waypoint_ele = 105.0;
inline constexpr std::int64_t waypoint_time_ms = track_start_ms - 300'000;
[[nodiscard]] inline std::vector<ir::Coord> synthetic_route() {
  return {ll(-29.45, 40.20), ll(-29.445, 40.202), ll(-29.44, 40.21)};
}

// --------------------------------------------------------------------------- comparison

/// Describes the first difference between two coordinate sequences, or "".
[[nodiscard]] inline std::string coords_diff(const std::vector<ir::Coord>& a, const std::vector<ir::Coord>& b,
                                             double tolerance = coord_tolerance) {
  if (a.size() != b.size()) return "point counts differ: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!(std::fabs(a[i].lon - b[i].lon) <= tolerance) || !(std::fabs(a[i].lat - b[i].lat) <= tolerance)) {
      std::ostringstream out;
      out.precision(12);
      out << "point " << i << ": (" << a[i].lon << ", " << a[i].lat << ") vs (" << b[i].lon << ", " << b[i].lat << ")";
      return out.str();
    }
  }
  return {};
}

/// Kind name of a geometry, for messages.
[[nodiscard]] inline std::string_view shape_name(const ir::Geometry& g) {
  static constexpr std::string_view names[] = {"Point", "LineString", "Polygon", "MultiPoint", "MultiLineString",
                                               "MultiPolygon", "GeometryCollection"};
  return names[g.shape.index()];
}

/// Rings compared as closed sequences; ring start and orientation must match.
[[nodiscard]] inline std::string geometry_diff(const ir::Geometry& a, const ir::Geometry& b,
                                               double tolerance = coord_tolerance) {
  if (a.shape.index() != b.shape.index()) {
    return std::string("geometry types differ: ") + std::string(shape_name(a)) + " vs " + std::string(shape_name(b));
  }
  const auto rings_diff = [&](const ir::Polygon& p, const ir::Polygon& q) -> std::string {
    if (p.rings.size() != q.rings.size()) return "ring counts differ";
    for (std::size_t r = 0; r < p.rings.size(); ++r) {
      std::string d = coords_diff(p.rings[r], q.rings[r], tolerance);
      if (!d.empty()) return "ring " + std::to_string(r) + ": " + d;
    }
    return {};
  };
  if (const auto* p = std::get_if<ir::Point>(&a.shape)) {
    return coords_diff({p->coord}, {std::get<ir::Point>(b.shape).coord}, tolerance);
  }
  if (const auto* l = std::get_if<ir::LineString>(&a.shape)) {
    return coords_diff(l->points, std::get<ir::LineString>(b.shape).points, tolerance);
  }
  if (const auto* p = std::get_if<ir::Polygon>(&a.shape)) return rings_diff(*p, std::get<ir::Polygon>(b.shape));
  if (const auto* m = std::get_if<ir::MultiPoint>(&a.shape)) {
    return coords_diff(m->points, std::get<ir::MultiPoint>(b.shape).points, tolerance);
  }
  if (const auto* m = std::get_if<ir::MultiLineString>(&a.shape)) {
    const auto& n = std::get<ir::MultiLineString>(b.shape);
    if (m->lines.size() != n.lines.size()) return "line counts differ";
    for (std::size_t i = 0; i < m->lines.size(); ++i) {
      std::string d = coords_diff(m->lines[i].points, n.lines[i].points, tolerance);
      if (!d.empty()) return "line " + std::to_string(i) + ": " + d;
    }
    return {};
  }
  if (const auto* m = std::get_if<ir::MultiPolygon>(&a.shape)) {
    const auto& n = std::get<ir::MultiPolygon>(b.shape);
    if (m->polygons.size() != n.polygons.size()) return "polygon counts differ";
    for (std::size_t i = 0; i < m->polygons.size(); ++i) {
      std::string d = rings_diff(m->polygons[i], n.polygons[i]);
      if (!d.empty()) return "polygon " + std::to_string(i) + ": " + d;
    }
    return {};
  }
  const auto& c = std::get<ir::GeometryCollection>(a.shape);
  const auto& d = std::get<ir::GeometryCollection>(b.shape);
  if (c.geometries.size() != d.geometries.size()) return "collection sizes differ";
  for (std::size_t i = 0; i < c.geometries.size(); ++i) {
    std::string diff = geometry_diff(c.geometries[i], d.geometries[i], tolerance);
    if (!diff.empty()) return "member " + std::to_string(i) + ": " + diff;
  }
  return {};
}

/// Compares the features of `actual` with `expected`: same count, geometry
/// within `tolerance`, and - when `compare_properties` - every expected
/// property present with an equal value (numbers within 1e-9 relative).
[[nodiscard]] inline std::string features_diff(const ir::Geo& expected, const ir::Geo& actual,
                                               bool compare_properties = true, double tolerance = coord_tolerance) {
  if (expected.features.size() != actual.features.size()) {
    return "feature counts differ: " + std::to_string(expected.features.size()) + " vs " +
           std::to_string(actual.features.size());
  }
  for (std::size_t i = 0; i < expected.features.size(); ++i) {
    std::string d = geometry_diff(expected.features[i].geometry, actual.features[i].geometry, tolerance);
    if (!d.empty()) return "feature " + std::to_string(i) + ": " + d;
    if (!compare_properties) continue;
    for (const auto& member : expected.features[i].properties.items()) {
      const Json* got = actual.features[i].properties.find(member.key());
      if (got == nullptr) return "feature " + std::to_string(i) + ": property '" + std::string(member.key()) + "' missing";
      const Json& want = member.value();
      bool same = want == *got;
      if (!same && want.is_number() && got->is_number()) {
        same = std::fabs(want.as_double() - got->as_double()) <= 1e-9 * std::fmax(1.0, std::fabs(want.as_double()));
      }
      if (!same) {
        return "feature " + std::to_string(i) + ": property '" + std::string(member.key()) + "' is " +
               util::dump_json(*got) + ", expected " + util::dump_json(want);
      }
    }
  }
  return {};
}

}  // namespace cvt::test::geo
