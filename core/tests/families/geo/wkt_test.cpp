// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "families/geo/geo.hpp"
#include "families/geo/wkt.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// The synthetic features without properties (WKT / WKB hold geometry only).
ir::Geo geometry_only() {
  ir::Geo geo = synthetic_features();
  for (auto& f : geo.features) {
    f.properties = Json::object();
    f.name.clear();
  }
  return geo;
}

// Elevation of every vertex is (longitude + 30) * 1000 (wkt_fixtures.py).
void check_z(const ir::Geo& geo) {
  for (const auto& f : geo.features) {
    bool ok = true;
    cvt::geo::for_each_coord(f.geometry, [&](const ir::Coord& c) {
      ok = ok && c.ele.has_value() && std::fabs(*c.ele - (c.lon + 30) * 1000) < 1e-6;
    });
    CHECK(ok);
  }
}

}  // namespace

TEST_CASE("geo/wkt: reads WKT written by shapely (GEOS)") {
  const auto geo = test::read_as<ir::Geo>("wkt", test::fixture("geo/wkt/features.wkt"));
  REQUIRE_OK(geo);
  CHECK_MESSAGE(features_diff(geometry_only(), *geo, false, 0.0).empty(), features_diff(geometry_only(), *geo, false, 0.0));
  const auto z = test::read_as<ir::Geo>("wkt", test::fixture("geo/wkt/features_z.wkt"));
  REQUIRE_OK(z);
  CHECK(features_diff(geometry_only(), *z, false, 0.0).empty());
  check_z(*z);
  const auto gc = test::read_as<ir::Geo>("wkt", test::fixture("geo/wkt/collection.wkt"));
  REQUIRE_OK(gc);
  REQUIRE(gc->features.size() == 1);
  const auto& members = std::get<ir::GeometryCollection>(gc->features[0].geometry.shape).geometries;
  REQUIRE(members.size() == 3);
  CHECK(std::holds_alternative<ir::GeometryCollection>(members[2].shape));
}

TEST_CASE("geo/wkb: reads WKB, ISO WKB Z and hex EWKB written by shapely") {
  for (const char* name : {"geo/wkt/features_le.wkb", "geo/wkt/features_be.wkb"}) {
    INFO(name);
    const auto geo = test::read_as<ir::Geo>("wkb", test::fixture(name));
    REQUIRE_OK(geo);
    CHECK(features_diff(geometry_only(), *geo, false, 0.0).empty());
  }
  const auto iso = test::read_as<ir::Geo>("wkb", test::fixture("geo/wkt/features_z_iso.wkb"));
  REQUIRE_OK(iso);
  CHECK(features_diff(geometry_only(), *iso, false, 0.0).empty());
  check_z(*iso);
  const auto hex = test::read_as<ir::Geo>("wkb-hex", test::fixture("geo/wkt/features_ewkb.hex"));
  REQUIRE_OK(hex);
  CHECK(features_diff(geometry_only(), *hex, false, 0.0).empty());
  check_z(*hex);
  CHECK(hex->crs.empty());  // SRID 4326 is the default
}

TEST_CASE("geo/wkt: parser handles the dialects") {
  int srid = 0;
  auto g = geo::geometry_from_wkt("SRID=3857;POINT(1 2)", &srid);
  REQUIRE_OK(g);
  CHECK(srid == 3857);
  g = geo::geometry_from_wkt("pointzm (1 2 3 4)");
  REQUIRE_OK(g);
  CHECK(std::get<ir::Point>(g->shape).coord.m == 4.0);
  g = geo::geometry_from_wkt("POINT M (1 2 7)");
  REQUIRE_OK(g);
  CHECK(std::get<ir::Point>(g->shape).coord.m == 7.0);
  CHECK_FALSE(std::get<ir::Point>(g->shape).coord.ele.has_value());
  g = geo::geometry_from_wkt("MULTIPOINT (1 2, 3 4)");
  REQUIRE_OK(g);
  CHECK(std::get<ir::MultiPoint>(g->shape).points.size() == 2);
  g = geo::geometry_from_wkt("LINESTRING EMPTY");
  REQUIRE_OK(g);
  CHECK(std::get<ir::LineString>(g->shape).points.empty());
  g = geo::geometry_from_wkt("TIN (((0 0, 1 0, 0 1, 0 0)), ((1 0, 1 1, 0 1, 1 0)))");
  REQUIRE_OK(g);
  CHECK(std::get<ir::MultiPolygon>(g->shape).polygons.size() == 2);
  CHECK_STATUS(geo::geometry_from_wkt("CIRCULARSTRING (0 0, 1 1, 2 0)"), Status::Decode);
  CHECK_STATUS(geo::geometry_from_wkt("POINT (1)"), Status::Decode);
  CHECK_STATUS(geo::geometry_from_wkt("POINT (1 2) junk"), Status::Decode);
  std::string deep;
  for (int i = 0; i < 40; ++i) deep += "GEOMETRYCOLLECTION (";
  deep += "POINT (1 2)";
  for (int i = 0; i < 40; ++i) deep += ")";
  CHECK_STATUS(geo::geometry_from_wkt(deep), Status::Limit);
}

TEST_CASE("geo/wkt: WKT, WKB and hex WKB round trips are exact") {
  const ir::Geo original = geometry_only();
  for (const char* format : {"wkt", "wkb", "wkb-hex"}) {
    INFO(format);
    std::vector<std::string> warnings;
    const auto bytes = test::write_from(format, synthetic_features(), "", &warnings);
    REQUIRE_OK(bytes);
    CHECK(!warnings.empty());  // attributes dropped
    const auto back = test::read_as<ir::Geo>(format, *bytes);
    REQUIRE_OK(back);
    if (std::string_view(format) == "wkb") {
      // Several features are written as one collection.
      REQUIRE(back->features.size() == 1);
      CHECK(std::get<ir::GeometryCollection>(back->features[0].geometry.shape).geometries.size() == 7);
    } else {
      CHECK(features_diff(original, *back, false, 0.0).empty());
    }
  }
  ir::Geo one;
  ir::Feature f;
  ir::Coord c = ll(1.5, -2.25);
  c.ele = 3;
  c.m = 4;
  f.geometry.shape = ir::Point{c};
  one.features.push_back(f);
  for (const char* options : {R"({"byte_order":"big"})", R"({"wkb_flavor":"ewkb"})", "{}"}) {
    const auto bytes = test::write_from("wkb", one, options);
    REQUIRE_OK(bytes);
    const auto back = test::read_as<ir::Geo>("wkb", *bytes);
    REQUIRE_OK(back);
    CHECK(std::get<ir::Point>(back->features[0].geometry.shape).coord == c);
  }
}

TEST_CASE("geo/wkt: target CRS and EWKT prefixes") {
  const auto bytes = test::write_from("wkt", geometry_only(), R"({"target_crs":"EPSG:3857","ewkt":true})");
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("SRID=3857;POINT"));
  const auto back = test::read_as<ir::Geo>("wkt", *bytes);
  REQUIRE_OK(back);
  CHECK(back->crs.empty());  // converted back to WGS 84 when read
  CHECK(features_diff(geometry_only(), *back, false, 1e-9).empty());
  CHECK_STATUS(test::write_from("wkt", geometry_only(), R"({"target_crs":"EPSG:9999999"})"), Status::InvalidArgument);
}

TEST_CASE("geo/wkt: detection, conversion and hostile input") {
  CHECK(test::convert(test::fixture("geo/wkt/features.wkt"), "", "geojson").ok());
  CHECK(test::convert(test::fixture("geo/wkt/features_ewkb.hex"), "", "geojson").ok());
  CHECK(test::convert(test::fixture("geo/wkt/features_le.wkb"), "", "geojson").ok());
  CHECK(test::convert(test::fixture("geo/wkt/features_le.wkb"), "", "wkt").ok());
  // Absurd counts never allocate.
  const Bytes huge = test::bytes(std::string_view("\x01\x02\x00\x00\x00\xff\xff\xff\x7f", 9));
  CHECK_STATUS(test::read_as<ir::Geo>("wkb", huge), Status::Decode);
  Bytes nested;
  for (int i = 0; i < 40; ++i) {
    const Bytes level = test::bytes(std::string_view("\x01\x07\x00\x00\x00\x01\x00\x00\x00", 9));
    nested.insert(nested.end(), level.begin(), level.end());
  }
  CHECK_STATUS(test::read_as<ir::Geo>("wkb", nested), Status::Limit);
  CHECK(test::fuzz_reader<ir::Geo>("wkt", test::fixture("geo/wkt/features_z.wkt")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("wkb", test::fixture("geo/wkt/features_be.wkb")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("wkb-hex", test::fixture("geo/wkt/features_ewkb.hex")) > 0);
}
