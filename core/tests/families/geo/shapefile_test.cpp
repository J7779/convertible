// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// OGR writes MULTILINESTRING / MULTIPOLYGON layers; single parts read back
// as LineString / Polygon. Layers come in archive order: lines, points, polygons.
ir::Geo ogr_order() {
  const ir::Geo s = synthetic_features();
  ir::Geo geo;
  for (int i : {2, 5, 0, 1, 3, 4}) geo.features.push_back(s.features[static_cast<std::size_t>(i)]);
  for (auto& f : geo.features) f.properties.set("active", f.properties["active"].as_bool() ? 1 : 0);
  return geo;
}

const ir::Feature* named(const ir::Geo& geo, std::string_view name) {
  for (const auto& f : geo.features) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("geo/shapefile: reads a zipped multi-layer shapefile written by OGR") {
  const auto geo = test::read_as<ir::Geo>("shp.zip", test::fixture("geo/shp/features_ogr.shp.zip"));
  REQUIRE_OK(geo);
  const std::string diff = features_diff(ogr_order(), *geo, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(geo->features[0].folder == "lines");
  CHECK(geo->features[2].folder == "points");
  CHECK(geo->features[2].properties["label"].as_string() == "Ünïcødé ✓ 東京");
  CHECK(geo->crs.empty());
}

TEST_CASE("geo/shapefile: a projected shapefile is converted to WGS 84 through its .prj") {
  const auto geo = test::read_as<ir::Geo>("shp.zip", test::fixture("geo/shp/polygons_utm.shp.zip"));
  REQUIRE_OK(geo);
  const ir::Geo s = synthetic_features();
  ir::Geo expected;
  expected.features = {s.features[3], s.features[4]};
  const std::string diff = features_diff(expected, *geo, false, 1e-8);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(geo->crs.empty());
}

TEST_CASE("geo/shapefile: Latin-1 attributes are decoded through the .cpg") {
  const auto geo = test::read_as<ir::Geo>("shp.zip", test::fixture("geo/shp/points_latin1.shp.zip"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 2);
  CHECK(geo->features[0].properties["label"].as_string() == "plain text");
  CHECK(geo->features[1].properties["label"].as_string() == "?mega field");  // GDAL could not encode the Omega
  CHECK(geo->features[1].properties["rank"] == Json(2));
}

TEST_CASE("geo/shapefile: Z, M and MultiPatch shapes written by pyshp") {
  const auto geo = test::read_as<ir::Geo>("shp.zip", test::fixture("geo/shp/zm_multipatch_pyshp.shp.zip"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 4);
  const ir::Feature* patch = named(*geo, "Patch");
  REQUIRE(patch != nullptr);
  const auto& mp = std::get<ir::MultiPolygon>(patch->geometry.shape);
  REQUIRE(mp.polygons.size() == 3);  // two strip triangles and one ring
  CHECK(mp.polygons[0].rings[0].size() == 4);
  CHECK(mp.polygons[0].rings[0][0].ele == 5.0);
  const ir::Feature* point = named(*geo, "Alpha Beacon");
  REQUIRE(point != nullptr);
  const auto& c = std::get<ir::Point>(point->geometry.shape).coord;
  CHECK(c.ele == 12.5);
  CHECK(c.m == 1.5);
  const ir::Feature* line = named(*geo, "Charlie Path");
  REQUIRE(line != nullptr);
  const auto& pts = std::get<ir::LineString>(line->geometry.shape).points;
  REQUIRE(pts.size() == 4);
  CHECK(pts[3].ele == 30.0);
  CHECK(pts[3].m == 3.0);
  const ir::Feature* polygon = named(*geo, "Delta Field");
  REQUIRE(polygon != nullptr);
  CHECK(geometry_diff(synthetic_features().features[3].geometry, polygon->geometry).empty());
  CHECK(std::get<ir::Polygon>(polygon->geometry.shape).rings[1][0].m.has_value());
}

TEST_CASE("geo/shapefile: a lone .shp gives geometry only") {
  std::vector<std::string> warnings;
  const auto geo = test::read_as<ir::Geo>("shp", test::fixture("geo/shp/points_alone.shp"), "", &warnings);
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 2);
  CHECK(geometry_diff(synthetic_features().features[0].geometry, geo->features[0].geometry).empty());
  CHECK(warnings.size() == 1);
  CHECK(test::convert(test::fixture("geo/shp/points_alone.shp"), "", "geojson").ok());
}

TEST_CASE("geo/shapefile: write and read back splits layers by type and keeps attributes") {
  ir::Geo geo = synthetic_features();
  geo.name = "synthetic";
  const auto bytes = test::write_from("shp.zip", geo);
  REQUIRE_OK(bytes);
  auto zip = util::ZipReader::open(*bytes);
  REQUIRE_OK(zip);
  CHECK(zip->contains("synthetic_points.shp"));
  CHECK(zip->contains("synthetic_multipoints.dbf"));
  CHECK(zip->contains("synthetic_lines.shx"));
  CHECK(zip->contains("synthetic_polygons.prj"));
  CHECK(zip->contains("synthetic_polygons.cpg"));
  const auto back = test::read_as<ir::Geo>("shp.zip", *bytes);
  REQUIRE_OK(back);
  REQUIRE(back->features.size() == 7);
  for (const auto& want : geo.features) {
    const ir::Feature* got = named(*back, want.name);
    REQUIRE(got != nullptr);
    ir::Geometry expected_geometry = want.geometry;
    if (const auto* ml = std::get_if<ir::MultiLineString>(&want.geometry.shape); ml && ml->lines.size() == 1) {
      expected_geometry.shape = ml->lines[0];
    }
    CHECK_MESSAGE(geometry_diff(expected_geometry, got->geometry, 0.0).empty(), want.name);
    ir::Geo a, b;
    a.features = {want};
    b.features = {*got};
    a.features[0].geometry = b.features[0].geometry;
    CHECK_MESSAGE(features_diff(a, b).empty(), features_diff(a, b));
  }
}

TEST_CASE("geo/shapefile: elevations, target CRS and option handling") {
  ir::Geo geo;
  ir::Feature f;
  f.properties = Json::object({{"name", "Track"}, {"note", std::string(300, 'x')}});
  f.geometry.shape = ir::LineString{synthetic_track().lines[0].points};
  geo.features.push_back(f);
  std::vector<std::string> warnings;
  const auto bytes = test::write_from("shp.zip", geo, R"({"target_crs":"EPSG:32626"})", &warnings);
  REQUIRE_OK(bytes);
  CHECK(warnings.size() == 1);  // the 300-byte text was cut
  auto zip = util::ZipReader::open(*bytes);
  REQUIRE_OK(zip);
  const auto prj = zip->read("layer.prj");
  REQUIRE_OK(prj);
  CHECK(test::str(*prj).find("WGS_1984_UTM_Zone_26N") != std::string::npos);
  const auto back = test::read_as<ir::Geo>("shp.zip", *bytes);
  REQUIRE_OK(back);
  const auto& pts = std::get<ir::LineString>(back->features[0].geometry.shape).points;
  CHECK(coords_diff(synthetic_track().lines[0].points, pts, 1e-8).empty());
  CHECK(pts[2].ele == 103.0);
  CHECK(back->features[0].properties["note"].as_string().size() == 254);
  const auto flat = test::write_from("shp.zip", geo, R"({"shp_z":false})");
  REQUIRE_OK(flat);
  const auto back_flat = test::read_as<ir::Geo>("shp.zip", *flat);
  REQUIRE_OK(back_flat);
  CHECK_FALSE(std::get<ir::LineString>(back_flat->features[0].geometry.shape).points[2].ele.has_value());
}

TEST_CASE("geo/shapefile: detection and conversion") {
  const Bytes zip = test::fixture("geo/shp/features_ogr.shp.zip");
  const test::Converted converted = test::convert(zip, "", "geojson", "", "data.zip");
  CHECK(converted.ok());
  CHECK(converted.format_id == "geojson");
  CHECK(test::convert(zip, "", "shp.zip").ok());  // the compress path
  CHECK(test::convert(test::fixture("geo/kml/features_libkml.kmz"), "", "shp.zip").ok());
}

TEST_CASE("geo/shapefile: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Geo>("shp", test::bytes("not a shapefile at all, sorry")), Status::Decode);
  Bytes shp = test::fixture("geo/shp/points_alone.shp");
  // A record that claims to be huge.
  shp[104] = std::byte{0x7f};
  CHECK_STATUS(test::read_as<ir::Geo>("shp", shp), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("shp", test::fixture("geo/shp/points_alone.shp")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("shp.zip", test::fixture("geo/shp/zm_multipatch_pyshp.shp.zip")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("shp.zip", test::fixture("geo/shp/points_latin1.shp.zip")) > 0);
}
