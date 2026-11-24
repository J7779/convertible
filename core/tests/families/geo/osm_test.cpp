// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// The features built from tools/sample.opl (see data/geo/SOURCES.md).
void check_sample(const ir::Geo& geo) {
  REQUIRE(geo.features.size() == 4);
  const ir::Feature& bench = geo.features[0];
  CHECK(bench.id == Json("node/10"));
  CHECK(bench.name == "Lima Bench");
  CHECK(bench.properties["amenity"] == Json("bench"));
  CHECK(geometry_diff(ir::Geometry{ir::Point{ll(-29.41, 40.02)}}, bench.geometry).empty());
  const ir::Feature& trail = geo.features[1];
  CHECK(trail.id == Json("way/20"));
  CHECK(geometry_diff(ir::Geometry{ir::LineString{{ll(-29.41, 40.02), ll(-29.42, 40.03), ll(-29.43, 40.035)}}},
                      trail.geometry).empty());
  const ir::Feature& hall = geo.features[2];
  CHECK(hall.properties["levels"] == Json("3"));
  const auto* building = std::get_if<ir::Polygon>(&hall.geometry.shape);
  REQUIRE(building != nullptr);
  CHECK(building->rings[0].size() == 5);
  CHECK(cvt::geo::signed_area(building->rings[0]) > 0);
  const ir::Feature& park = geo.features[3];
  CHECK(park.id == Json("relation/30"));
  CHECK(park.name == "Oscar Park");
  CHECK_FALSE(park.properties.contains("type"));
  const auto* polygon = std::get_if<ir::Polygon>(&park.geometry.shape);
  REQUIRE(polygon != nullptr);
  REQUIRE(polygon->rings.size() == 2);  // outer joined from two ways, one hole
  CHECK(polygon->rings[0].size() == 5);
  CHECK_APPROX(cvt::geo::area_m2(park.geometry), 0.01 * 0.01 * 0.64 * 111195.0 * 111195.0 * 0.7657, 1e4);
}

}  // namespace

TEST_CASE("geo/osm: reads OSM XML written by osmium") {
  const auto geo = test::read_as<ir::Geo>("osm", test::fixture("geo/osm/sample.osm"));
  REQUIRE_OK(geo);
  check_sample(*geo);
}

TEST_CASE("geo/osm: reads OSM PBF (dense nodes, zlib blobs) written by osmium") {
  const auto geo = test::read_as<ir::Geo>("osm.pbf", test::fixture("geo/osm/sample.osm.pbf"));
  REQUIRE_OK(geo);
  check_sample(*geo);
}

TEST_CASE("geo/osm: write OSM XML and read it back") {
  const auto source = test::read_as<ir::Geo>("osm", test::fixture("geo/osm/sample.osm"));
  REQUIRE_OK(source);
  const auto bytes = test::write_from("osm", *source);
  REQUIRE_OK(bytes);
  const std::string text = test::str(*bytes);
  CHECK(text.find("<relation id=\"") != std::string::npos);
  CHECK(text.find("k=\"type\" v=\"multipolygon\"") != std::string::npos);
  const auto back = test::read_as<ir::Geo>("osm", *bytes);
  REQUIRE_OK(back);
  REQUIRE(back->features.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(geometry_diff(source->features[i].geometry, back->features[i].geometry, 1e-7).empty());
    CHECK(back->features[i].name == source->features[i].name);
  }
  // The synthetic features: lines, polygons with holes and multipolygons.
  const auto features = test::write_from("osm", synthetic_features());
  REQUIRE_OK(features);
  const auto fback = test::read_as<ir::Geo>("osm", *features);
  REQUIRE_OK(fback);
  CHECK(fback->features.size() == 2 + 1 + 1 + 1 + 2 + 2);  // MultiLineString -> 2 ways, MultiPoint -> 2 nodes
}

TEST_CASE("geo/osm: detection and conversion") {
  CHECK(test::convert(test::fixture("geo/osm/sample.osm"), "", "geojson").ok());
  const test::Converted pbf = test::convert(test::fixture("geo/osm/sample.osm.pbf"), "", "geojson");
  CHECK(pbf.ok());
  CHECK(test::convert(test::fixture("geo/osm/sample.osm.pbf"), "", "osm").ok());
}

TEST_CASE("geo/osm: hostile PBF input fails cleanly") {
  Bytes pbf = test::fixture("geo/osm/sample.osm.pbf");
  // A blob header length far beyond the limit.
  Bytes huge = pbf;
  huge[0] = std::byte{0x7f};
  CHECK_STATUS(test::read_as<ir::Geo>("osm.pbf", huge), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("osm.pbf", ByteView(pbf).first(pbf.size() - 3)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("osm.pbf", pbf) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("osm", test::fixture("geo/osm/sample.osm")) > 0);
}
