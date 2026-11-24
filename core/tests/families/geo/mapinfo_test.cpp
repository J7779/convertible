// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// OGR writes the MultiPoint as two Points and booleans as integers.
ir::Geo ogr_expected() {
  ir::Geo geo = synthetic_features();
  ir::Feature golf = geo.features.back();
  geo.features.pop_back();
  for (const auto& c : std::get<ir::MultiPoint>(golf.geometry.shape).points) {
    ir::Feature p = golf;
    p.geometry.shape = ir::Point{c};
    geo.features.push_back(p);
  }
  for (auto& f : geo.features) f.properties.set("active", f.properties["active"].as_bool() ? 1 : 0);
  return geo;
}

}  // namespace

TEST_CASE("geo/mapinfo: reads MIF/MID written by OGR") {
  const auto geo = test::read_as<ir::Geo>("mapinfo.zip", test::fixture("geo/mapinfo/features_mif.zip"));
  REQUIRE_OK(geo);
  const std::string diff = features_diff(ogr_expected(), *geo, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(geo->features[0].properties["label"] == Json("Ünïcødé ✓ 東京"));
  CHECK(geo->features[3].style.fill.empty());  // Brush pattern 1 is hollow
  CHECK(geo->features[2].style.stroke == "#000000");
  CHECK(geo->name == "features");
}

TEST_CASE("geo/mapinfo: a lone MIF gives geometry and a note about the MID") {
  std::vector<std::string> warnings;
  const auto geo = test::read_as<ir::Geo>("mif", test::fixture("geo/mapinfo/features_alone.mif"), "", &warnings);
  REQUIRE_OK(geo);
  CHECK(features_diff(ogr_expected(), *geo, false, 0.0).empty());
  CHECK(warnings.size() == 1);
  CHECK(test::convert(test::fixture("geo/mapinfo/features_alone.mif"), "", "geojson").ok());
}

TEST_CASE("geo/mapinfo: native TAB is recognised and reported") {
  const auto tab = test::read_as<ir::Geo>("mapinfo.zip", test::fixture("geo/mapinfo/features_tab.zip"));
  CHECK_STATUS(tab, Status::Decode);
  CHECK(tab.error().message.find("TAB") != std::string::npos);
}

TEST_CASE("geo/mapinfo: write MIF/MID and read it back") {
  ir::Geo geo = synthetic_features();
  geo.name = "synthetic";
  const auto bytes = test::write_from("mapinfo.zip", geo);
  REQUIRE_OK(bytes);
  auto zip = util::ZipReader::open(*bytes);
  REQUIRE_OK(zip);
  CHECK(zip->contains("synthetic.mif"));
  CHECK(zip->contains("synthetic.mid"));
  const auto back = test::read_as<ir::Geo>("mapinfo.zip", *bytes);
  REQUIRE_OK(back);
  const std::string diff = features_diff(geo, *back, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(test::convert(test::fixture("geo/mapinfo/features_mif.zip"), "", "mapinfo.zip").ok());
}

TEST_CASE("geo/mapinfo: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Geo>("mif", test::bytes("Version 300\nColumns 1\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("mif", test::bytes("Version 300\nData\nPline 999999999\n1 2\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("mif", test::bytes("Version 300\nData\nBlob 1 2\n")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("mif", test::fixture("geo/mapinfo/features_alone.mif")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("mapinfo.zip", test::fixture("geo/mapinfo/features_mif.zip")) > 0);
}
