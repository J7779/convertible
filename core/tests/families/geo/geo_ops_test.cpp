// Coordinate operations: projections against PROJ (pyproj) reference values,
// geodesic measurements, simplification invariants, track statistics.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include <numbers>

#include "util/text.hpp"

using namespace cvt;
using namespace cvt::test::geo;
namespace g = cvt::geo;

namespace {

struct Reference {
  std::string kind;
  std::vector<double> values;
  std::string label;
};

std::vector<Reference> references() {
  std::vector<Reference> out;
  const std::string text = test::fixture_text("geo/json/proj_reference.txt");  // split_lines() returns views into it
  for (std::string_view line : util::split_lines(text)) {
    const auto parts = util::split(util::trim(line), ' ', false);
    if (parts.empty()) continue;
    Reference r;
    r.kind = std::string(parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i) {
      if (auto v = util::parse_double(parts[i])) r.values.push_back(*v);
      else r.label = std::string(parts[i]);
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace

TEST_CASE("geo/ops: projections match PROJ (pyproj) to the millimetre") {
  std::size_t checked = 0;
  for (const Reference& r : references()) {
    if (!r.kind.starts_with("EPSG:")) continue;
    INFO(r.kind, " ", r.values[0], " ", r.values[1]);
    const auto crs = g::parse_crs(r.kind);
    REQUIRE(crs.has_value());
    const g::XY xy = g::project(*crs, r.values[0], r.values[1]);
    // British National Grid goes through the inverse Helmert parameters, as published, so allow 5 cm.
    const double tolerance = crs->kind == g::CrsKind::BritishNationalGrid ? 0.05 : 1e-3;
    CHECK_APPROX(xy.x, r.values[2], tolerance);
    CHECK_APPROX(xy.y, r.values[3], tolerance);
    const g::XY back = g::unproject(*crs, xy.x, xy.y);
    CHECK_APPROX(back.x, r.values[0], crs->kind == g::CrsKind::BritishNationalGrid ? 1e-6 : 1e-9);
    CHECK_APPROX(back.y, r.values[1], crs->kind == g::CrsKind::BritishNationalGrid ? 1e-6 : 1e-9);
    ++checked;
  }
  CHECK(checked == 13);
}

TEST_CASE("geo/ops: geodesic distances and areas match PROJ's geodesics") {
  for (const Reference& r : references()) {
    if (r.kind == "distance") {
      INFO(r.values[0], " ", r.values[1], " -> ", r.values[2], " ", r.values[3]);
      const double d = g::geodesic_distance(r.values[0], r.values[1], r.values[2], r.values[3]);
      // Vincenty is exact to well under a millimetre except near the antipode, where it falls back to a sphere.
      const bool antipodal = std::fabs(r.values[2] - r.values[0]) > 179;
      CHECK_APPROX(d, r.values[4], antipodal ? r.values[4] * 0.006 : 1e-3);
    } else if (r.kind == "area") {
      INFO(r.label);
      const ir::Geo geo = synthetic_features();
      const std::string name = util::replace_all(r.label, "_", " ");
      for (const auto& f : geo.features) {
        if (f.name != name) continue;
        CHECK_APPROX(g::area_m2(f.geometry), r.values[0], r.values[0] * 0.003);  // authalic sphere vs ellipsoid
        // PROJ reports the perimeter of the outer rings.
        double perimeter = 0;
        const auto outer = [&](const ir::Polygon& p) {
          for (std::size_t i = 1; i < p.rings[0].size(); ++i) perimeter += g::geodesic_distance(p.rings[0][i - 1], p.rings[0][i]);
        };
        if (const auto* p = std::get_if<ir::Polygon>(&f.geometry.shape)) outer(*p);
        if (const auto* m = std::get_if<ir::MultiPolygon>(&f.geometry.shape)) {
          for (const auto& p : m->polygons) outer(p);
        }
        CHECK_APPROX(perimeter, r.values[1], 1e-2);
      }
    }
  }
}

TEST_CASE("geo/ops: CRS identifiers and WKT are recognised") {
  CHECK(g::parse_crs("urn:ogc:def:crs:EPSG::4326")->kind == g::CrsKind::Geographic);
  CHECK(g::parse_crs("http://www.opengis.net/def/crs/EPSG/0/32633")->zone == 33);
  CHECK_FALSE(g::parse_crs("EPSG:32733")->north);
  CHECK(g::parse_crs("EPSG:900913")->epsg == 3857);
  CHECK(g::parse_crs("EPSG:25832")->datum == g::Datum::Etrs89);
  CHECK(g::parse_crs("OGC:CRS84")->kind == g::CrsKind::Geographic);
  CHECK_FALSE(g::parse_crs("EPSG:2056").has_value());  // Swiss LV95: not supported
  for (const auto& crs : {*g::parse_crs("EPSG:3857"), *g::parse_crs("EPSG:32626"), *g::parse_crs("EPSG:32733"),
                          *g::parse_crs("EPSG:26915"), *g::parse_crs("EPSG:25832"), *g::parse_crs("EPSG:27700"),
                          *g::parse_crs("EPSG:4326")}) {
    INFO(g::crs_id(crs));
    CHECK(g::parse_crs(g::crs_esri_wkt(crs)) == crs);
    CHECK(g::parse_crs(g::crs_ogc_wkt(crs)) == crs);
  }
  CHECK(g::utm_crs_for(-29.48, 40.12).epsg == 32626);
  CHECK(g::utm_crs_for(5.5, 60.0).zone == 32);  // Norway exception
  CHECK(g::utm_crs_for(15.0, 78.0).zone == 33);  // Svalbard exception
}

TEST_CASE("geo/ops: Douglas-Peucker and Visvalingam simplification invariants") {
  // A wiggly line: 2000 points, deviation of +-3 m around a straight east-west line.
  std::vector<ir::Coord> line;
  for (int i = 0; i < 2000; ++i) {
    ir::Coord c = ll(-29.5 + i * 1e-4, 40.0 + ((i % 7) - 3) * 3e-5 * ((i % 2) ? 1 : -1) / 3.3);
    line.push_back(c);
  }
  for (auto method : {g::SimplifyMethod::DouglasPeucker, g::SimplifyMethod::Visvalingam}) {
    const auto kept = g::simplify_indices(line, 10.0, method, false, false);
    CHECK(kept.front() == 0);
    CHECK(kept.back() == line.size() - 1);
    CHECK(kept.size() < line.size() / (method == g::SimplifyMethod::DouglasPeucker ? 10 : 4));
    CHECK(g::simplify_indices(line, 0.0, method, false, false).size() == line.size());
  }
  // DP keeps every point that deviates more than the tolerance.
  std::vector<ir::Coord> spike = {ll(0, 0), ll(0.001, 0.0005), ll(0.002, 0)};
  CHECK(g::simplify_indices(spike, 10.0, g::SimplifyMethod::DouglasPeucker, false, false).size() == 3);
  CHECK(g::simplify_indices(spike, 100.0, g::SimplifyMethod::DouglasPeucker, false, false).size() == 2);
  // Rings never collapse below four points and never cross themselves.
  ir::Geo geo;
  ir::Feature f;
  std::vector<ir::Coord> ring;
  for (int i = 0; i < 360; ++i) {
    const double t = i * std::numbers::pi / 180;
    const double r = 0.01 * (1 + 0.3 * std::sin(7 * t));  // a star-like ring
    ring.push_back(ll(r * std::cos(t), 40 + r * std::sin(t)));
  }
  ring.push_back(ring.front());
  f.geometry.shape = ir::Polygon{{ring}};
  geo.features.push_back(f);
  for (double tolerance : {1.0, 50.0, 400.0, 5000.0}) {
    ir::Geo copy = geo;
    g::simplify(copy, tolerance, g::SimplifyMethod::DouglasPeucker);
    const auto& out = std::get<ir::Polygon>(copy.features[0].geometry.shape).rings[0];
    INFO("tolerance ", tolerance);
    CHECK(out.size() >= 4);
    CHECK(out.front() == out.back());
    CHECK_FALSE(g::ring_self_intersects(out));
    ir::Geo vw = geo;
    g::simplify(vw, tolerance, g::SimplifyMethod::Visvalingam);
    const auto& vout = std::get<ir::Polygon>(vw.features[0].geometry.shape).rings[0];
    CHECK(vout.size() >= 4);
    CHECK_FALSE(g::ring_self_intersects(vout));
  }
  CHECK(g::ring_self_intersects(closed({ll(0, 0), ll(1, 1), ll(1, 0), ll(0, 1)})));  // a bow tie
}

TEST_CASE("geo/ops: lower quality makes smaller GeoJSON (the compress path)") {
  // A dense track: 3000 points.
  ir::Geo geo;
  ir::Feature f;
  ir::LineString line;
  for (int i = 0; i < 3000; ++i) line.points.push_back(ll(-29.5 + i * 2e-5, 40 + 1e-4 * std::sin(i / 40.0)));
  f.geometry.shape = line;
  geo.features.push_back(f);
  const auto high = test::write_from("geojson", geo);
  const auto balanced = test::write_from("geojson", geo, R"({"preset":"balanced"})");
  const auto small = test::write_from("geojson", geo, R"({"quality":20})");
  REQUIRE_OK(high);
  REQUIRE_OK(balanced);
  REQUIRE_OK(small);
  CHECK(balanced->size() < high->size());
  CHECK(small->size() < balanced->size());
  // The simplified line stays within the derived tolerance (quality 20: 0.25 * 2^7 = 32 m).
  const auto back = test::read_as<ir::Geo>("geojson", *small);
  REQUIRE_OK(back);
  const auto& pts = std::get<ir::LineString>(back->features[0].geometry.shape).points;
  CHECK(pts.size() < 200);
  CHECK(pts.front().lon == -29.5);
  const auto explicit_tolerance = test::write_from("geojson", geo, R"({"simplify_tolerance":1,"simplify_method":"visvalingam"})");
  REQUIRE_OK(explicit_tolerance);
  CHECK(explicit_tolerance->size() < high->size());
}

TEST_CASE("geo/ops: track statistics of the synthetic run") {
  const ir::Geometry track{synthetic_track()};
  const g::TrackStats s = g::track_stats(track);
  CHECK(s.points == 11);
  CHECK(s.segments == 2);
  double expected = 0;
  const auto t = synthetic_track();
  for (const auto& l : t.lines) {
    for (std::size_t i = 1; i < l.points.size(); ++i) expected += g::geodesic_distance(l.points[i - 1], l.points[i]);
  }
  CHECK_APPROX(s.distance_m, expected, 1e-9);
  CHECK(*s.duration_s == 340.0);
  CHECK(*s.moving_time_s == 90.0);
  CHECK_APPROX(*s.average_speed_mps, s.distance_m / 90.0, 1e-12);
  CHECK_APPROX(s.elevation_gain_m, 7.5, 1e-9);
  CHECK_APPROX(s.elevation_loss_m, 8.0, 1e-9);
  CHECK(*s.max_heart_rate == 134.0);
  CHECK_APPROX(*s.average_heart_rate, (120 + 121 + 122 + 123 + 124 + 125 + 130 + 131 + 132 + 133 + 134) / 11.0, 1e-9);
  // Hysteresis ignores small wiggles.
  const g::TrackStats smooth = g::track_stats(track, 5.0);
  CHECK(smooth.elevation_gain_m <= s.elevation_gain_m);
}

TEST_CASE("geo/ops: rounding, orientation, bounding boxes and reprojection of a collection") {
  ir::Geo geo = synthetic_features();
  g::round_coordinates(geo, 2);
  CHECK(std::get<ir::Point>(geo.features[0].geometry.shape).coord.lon == -29.48);
  ir::Geometry poly = synthetic_features().features[3].geometry;
  g::orient_rings(poly, false);
  CHECK(g::signed_area(std::get<ir::Polygon>(poly.shape).rings[0]) < 0);
  CHECK(g::signed_area(std::get<ir::Polygon>(poly.shape).rings[1]) > 0);
  const g::BoundingBox box = g::bounding_box(synthetic_features());
  CHECK(box.min_lon == -29.5);
  CHECK(box.max_lat == 40.31);
  ir::Geo projected = synthetic_features();
  REQUIRE_OK(g::reproject(projected, *g::parse_crs("EPSG:32626")));
  CHECK(projected.crs == "EPSG:32626");
  REQUIRE_OK(g::reproject(projected, g::Crs{}));
  CHECK(projected.crs.empty());
  CHECK(features_diff(synthetic_features(), projected, true, 1e-9).empty());
  CHECK(g::format_time(*g::parse_time("2024-03-10T09:00:00.25+01:00")) == "2024-03-10T08:00:00.250Z");
  CHECK(g::parse_time("2024/03/10 08:00:00+00") == track_start_ms);
  CHECK_FALSE(g::parse_time("2024-13-10T00:00:00Z").has_value());
  CHECK(g::format_number(0.1 + 0.2, 6) == "0.3");
  CHECK(g::format_number(-0.0000001, 3) == "0");
}
