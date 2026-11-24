// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

TEST_CASE("geo/geobuf: decodes a file written by the python geobuf package") {
  const auto geo = test::read_as<ir::Geo>("geobuf", test::fixture("geo/geobuf/features.geobuf"));
  REQUIRE_OK(geo);
  const std::string diff = features_diff(synthetic_features(), *geo, true, 1e-12);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(geo->features[0].properties["active"] == Json(true));
  CHECK(geo->features[3].properties["height"] == Json(100.75));
}

TEST_CASE("geo/geobuf: write and read back, ids, 3D and collections, precision option") {
  ir::Geo geo = synthetic_features();
  geo.features[0].id = Json("alpha");
  geo.features[1].id = Json(-7);
  ir::Feature extra;
  ir::Coord c = ll(1.5, 2.25);
  c.ele = 3.125;
  extra.geometry.shape = ir::GeometryCollection{{ir::Geometry{ir::Point{c}}, ir::Geometry{ir::MultiPoint{{c, ll(2, 3)}}}}};
  extra.properties = Json::object({{"nested", Json::object({{"a", 1}})}, {"none", nullptr}, {"neg", -5}});
  geo.features.push_back(extra);
  const auto bytes = test::write_from("geobuf", geo);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("geobuf", *bytes);
  REQUIRE_OK(back);
  const std::string diff = features_diff(geo, *back, true, 1e-12);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(back->features[0].id == Json("alpha"));
  CHECK(back->features[1].id == Json(-7));
  const auto& gc = std::get<ir::GeometryCollection>(back->features.back().geometry.shape);
  CHECK(std::get<ir::Point>(gc.geometries[0].shape).coord.ele == 3.125);
  CHECK(back->features.back().properties["neg"] == Json(-5));
  const auto coarse = test::write_from("geobuf", geo, R"({"geobuf_precision":3})");
  REQUIRE_OK(coarse);
  CHECK(coarse->size() < bytes->size());
  // Smaller than the same data as GeoJSON.
  CHECK(bytes->size() < test::write_from("geojson", geo)->size());
  CHECK(test::convert(test::fixture("geo/geobuf/features.geobuf"), "", "geojson", "", "features.geobuf").ok());
}

TEST_CASE("geo/geobuf: damaged input fails cleanly") {
  const Bytes geobuf = test::fixture("geo/geobuf/features.geobuf");
  CHECK_STATUS(test::read_as<ir::Geo>("geobuf", ByteView(geobuf).first(geobuf.size() / 2)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("geobuf", geobuf) > 0);
}
