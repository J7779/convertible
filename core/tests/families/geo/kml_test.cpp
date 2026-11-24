// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// Properties as OGR writes them: booleans become 0/1 integers.
ir::Geo expected_with_int_booleans() {
  ir::Geo geo = synthetic_features();
  for (auto& f : geo.features) f.properties.set("active", f.properties["active"].as_bool() ? 1 : 0);
  return geo;
}

}  // namespace

TEST_CASE("geo/kml: reads KML written by GDAL's KML and LIBKML drivers") {
  for (const char* name : {"geo/kml/features_ogr.kml", "geo/kml/features_libkml.kml"}) {
    INFO(name);
    const auto geo = test::read_as<ir::Geo>("kml", test::fixture(name));
    REQUIRE_OK(geo);
    const std::string diff = features_diff(expected_with_int_booleans(), *geo);
    CHECK_MESSAGE(diff.empty(), diff);
    CHECK(geo->features[3].properties["height"] == Json(100.75));
    CHECK(geo->features[3].properties["rank"].is_integer());
  }
}

TEST_CASE("geo/kml: reads a KMZ written by LIBKML") {
  const auto geo = test::read_as<ir::Geo>("kmz", test::fixture("geo/kml/features_libkml.kmz"));
  REQUIRE_OK(geo);
  const std::string diff = features_diff(expected_with_int_booleans(), *geo);
  CHECK_MESSAGE(diff.empty(), diff);
}

TEST_CASE("geo/kml: reads gx:Track with sensor arrays and folders from GPSBabel") {
  const auto geo = test::read_as<ir::Geo>("kml", test::fixture("geo/kml/track_gpsbabel.kml"));
  REQUIRE_OK(geo);
  const ir::Feature* track = nullptr;
  for (const auto& f : geo->features) {
    if (f.role == ir::FeatureRole::Track) track = &f;
  }
  REQUIRE(track != nullptr);
  CHECK(track->name == "Hotel Run");
  CHECK(track->folder == "Tracks");
  const auto& lines = std::get<ir::MultiLineString>(track->geometry.shape).lines;
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0].points.size() == 11);
  for (int i = 0; i < 11; ++i) {
    const ir::Coord want = track_point(i < 6 ? 0 : 1, i < 6 ? i : i - 6);
    const ir::Coord& got = lines[0].points[static_cast<std::size_t>(i)];
    CHECK_APPROX(got.lon, want.lon, 1e-6);
    CHECK_APPROX(got.lat, want.lat, 1e-6);
    CHECK(got.time_ms == want.time_ms);
    CHECK(got.heart_rate == want.heart_rate);
    CHECK(got.cadence == want.cadence);
    CHECK(got.temperature == want.temperature);
  }
}

TEST_CASE("geo/kml: write and read back keeps geometry, typed properties, folders, styles and tracks") {
  ir::Geo geo = synthetic_features();
  geo.name = "Synthetic";
  geo.features[0].folder = "Marks/Beacons";
  geo.features[3].style.fill = "#336699";
  geo.features[3].style.fill_opacity = 0.5;
  geo.features[3].style.stroke = "#ff0000";
  geo.features[3].style.stroke_width = 2;
  ir::Feature track;
  track.name = "Hotel Run";
  track.role = ir::FeatureRole::Track;
  track.geometry.shape = synthetic_track();
  geo.features.push_back(track);
  const auto bytes = test::write_from("kml", geo);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("kml", *bytes);
  REQUIRE_OK(back);
  CHECK(back->name == "Synthetic");
  ir::Geo expected = synthetic_features();
  expected.features.push_back(track);
  const std::string diff = features_diff(expected, *back, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(back->features[0].folder == "Marks/Beacons");
  CHECK(back->features[0].properties["active"] == Json(true));
  const ir::Style& style = back->features[3].style;
  CHECK(style.fill == "#336699");
  CHECK_APPROX(style.fill_opacity, 0.5, 1.0 / 255);  // KML stores opacity in 8 bits
  CHECK(style.stroke == "#ff0000");
  CHECK(style.stroke_width == 2.0);
  const auto& line = std::get<ir::MultiLineString>(back->features.back().geometry.shape).lines[1].points;
  CHECK(line[2] == track_point(1, 2));
  // The same as plain LineStrings without time.
  const auto plain = test::write_from("kml", geo, R"({"kml_tracks":"linestring"})");
  REQUIRE_OK(plain);
  CHECK(test::str(*plain).find("gx:Track") == std::string::npos);
}

TEST_CASE("geo/kmz: write and read back keeps resources") {
  ir::Geo geo = synthetic_features();
  geo.resources.push_back({"files/icon.png", test::bytes("not really a png")});
  const auto bytes = test::write_from("kmz", geo);
  REQUIRE_OK(bytes);
  auto zip = util::ZipReader::open(*bytes);
  REQUIRE_OK(zip);
  CHECK(zip->entries()[0].name == "doc.kml");
  const auto back = test::read_as<ir::Geo>("kmz", *bytes);
  REQUIRE_OK(back);
  CHECK(features_diff(synthetic_features(), *back).empty());
  REQUIRE(back->resources.size() == 1);
  CHECK(back->resources[0].path == "files/icon.png");
  CHECK(test::str(back->resources[0].data) == "not really a png");
}

TEST_CASE("geo/kml: network links are never followed") {
  const std::string kml =
      "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><NetworkLink><Link><href>http://example.invalid/x.kml"
      "</href></Link></NetworkLink><Placemark><name>A</name><Point><coordinates>1, 2</coordinates></Point>"
      "</Placemark></Document></kml>";
  std::vector<std::string> warnings;
  const auto geo = test::read_as<ir::Geo>("kml", test::bytes(kml), "", &warnings);
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 1);
  CHECK(std::get<ir::Point>(geo->features[0].geometry.shape).coord.lat == 2.0);
  CHECK(warnings.size() == 1);
}

TEST_CASE("geo/kml: detection and conversion") {
  const Bytes kml = test::fixture("geo/kml/features_libkml.kml");
  const Bytes kmz = test::fixture("geo/kml/features_libkml.kmz");
  CHECK(test::convert(kml, "", "geojson").ok());
  const test::Converted from_kmz = test::convert(kmz, "", "geojson");
  CHECK(from_kmz.ok());
  CHECK(test::convert(kmz, "", "kmz", "", "doc.kmz").ok());
  CHECK(test::convert(test::fixture("geo/synthetic_track.gpx"), "gpx", "kml").ok());
}

TEST_CASE("geo/kml: damaged and hostile input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Geo>("kml", test::bytes("<gpx/>")), Status::Decode);
  std::string deep = "<kml><Document><Placemark>";
  for (int i = 0; i < 40; ++i) deep += "<MultiGeometry>";
  deep += "<Point><coordinates>1,2</coordinates></Point>";
  for (int i = 0; i < 40; ++i) deep += "</MultiGeometry>";
  deep += "</Placemark></Document></kml>";
  CHECK_STATUS(test::read_as<ir::Geo>("kml", test::bytes(deep)), Status::Limit);
  const std::string laughs =
      "<?xml version=\"1.0\"?><!DOCTYPE kml [<!ENTITY a \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\">"
      "<!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\"><!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">"
      "<!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">]><kml><Document><name>&d;&d;&d;&d;</name>"
      "</Document></kml>";
  CHECK_FALSE(test::read_as<ir::Geo>("kml", test::bytes(laughs)).has_value());
  CHECK_STATUS(test::read_as<ir::Geo>("kmz", test::bytes("PK\x03\x04 nothing")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("kml", test::fixture("geo/kml/features_ogr.kml")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("kmz", test::fixture("geo/kml/features_libkml.kmz")) > 0);
}
