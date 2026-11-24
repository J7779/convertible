// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// GDAL orders indexed files along a Hilbert curve: compare features by name.
void check_by_name(const ir::Geo& got) {
  const ir::Geo expected = synthetic_features();
  REQUIRE(got.features.size() == expected.features.size());
  for (const auto& want : expected.features) {
    const ir::Feature* match = nullptr;
    for (const auto& f : got.features) {
      if (f.name == want.name) match = &f;
    }
    REQUIRE_MESSAGE(match != nullptr, want.name);
    ir::Geo a, b;
    a.features = {want};
    b.features = {*match};
    const std::string diff = features_diff(a, b, true, 0.0);
    CHECK_MESSAGE(diff.empty(), want.name, ": ", diff);
  }
}

}  // namespace

TEST_CASE("geo/fgb: reads FlatGeobuf written by GDAL, with and without the spatial index") {
  for (const char* name : {"geo/fgb/features_indexed.fgb", "geo/fgb/features_noindex.fgb"}) {
    INFO(name);
    const auto geo = test::read_as<ir::Geo>("fgb", test::fixture(name));
    REQUIRE_OK(geo);
    check_by_name(*geo);
    CHECK(geo->crs.empty());
  }
}

TEST_CASE("geo/fgb: write and read back exactly, mixed types, Z and M, projected") {
  ir::Geo geo = synthetic_features();
  geo.name = "synthetic";
  ir::Feature z;
  ir::Coord c = ll(-29.0, 40.5);
  c.ele = 12.25;
  c.m = 3;
  z.geometry.shape = ir::GeometryCollection{{ir::Geometry{ir::Point{c}}, ir::Geometry{ir::LineString{{c, ll(-28.9, 40.6)}}}}};
  z.properties = Json::object({{"name", "Zulu"}, {"tags", Json::array({1, "two"})}});
  geo.features.push_back(z);
  const auto bytes = test::write_from("fgb", geo);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("fgb", *bytes);
  REQUIRE_OK(back);
  CHECK(back->name == "synthetic");
  ir::Geo expected = geo;
  const std::string diff = features_diff(expected, *back, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  const auto& members = std::get<ir::GeometryCollection>(back->features.back().geometry.shape).geometries;
  CHECK(std::get<ir::Point>(members[0].shape).coord.ele == 12.25);
  CHECK(std::get<ir::Point>(members[0].shape).coord.m == 3.0);
  CHECK(back->features.back().properties["tags"][1] == Json("two"));
  const auto projected = test::write_from("fgb", synthetic_features(), R"({"target_crs":"EPSG:3857"})");
  REQUIRE_OK(projected);
  const auto pback = test::read_as<ir::Geo>("fgb", *projected);
  REQUIRE_OK(pback);
  CHECK(features_diff(synthetic_features(), *pback, true, 1e-9).empty());
  CHECK(test::convert(test::fixture("geo/fgb/features_indexed.fgb"), "", "fgb").ok());
  CHECK(test::convert(test::fixture("geo/fgb/features_indexed.fgb"), "", "geojson").ok());
}

TEST_CASE("geo/fgb: damaged and hostile files fail cleanly") {
  Bytes fgb = test::fixture("geo/fgb/features_noindex.fgb");
  CHECK_STATUS(test::read_as<ir::Geo>("fgb", ByteView(fgb).first(30)), Status::Decode);
  Bytes huge = fgb;
  huge[8] = std::byte{0xff};
  huge[11] = std::byte{0x7f};
  CHECK_STATUS(test::read_as<ir::Geo>("fgb", huge), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("fgb", fgb) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("fgb", test::fixture("geo/fgb/features_indexed.fgb")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("fgb", *test::write_from("fgb", synthetic_features())) > 0);
}
