// Tests of the small text formats: CSV with coordinates, encoded polylines,
// geohashes and Plus Codes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

ir::Geo with_int_booleans(ir::Geo geo) {
  for (auto& f : geo.features) f.properties.set("active", f.properties["active"].as_bool() ? 1 : 0);
  return geo;
}

}  // namespace

// --------------------------------------------------------------------------- geocsv

TEST_CASE("geo/geocsv: reads GDAL CSV with WKT and X/Y columns") {
  const auto wkt = test::read_as<ir::Geo>("geocsv", test::fixture("geo/csv/features_wkt.csv"));
  REQUIRE_OK(wkt);
  const std::string diff = features_diff(with_int_booleans(synthetic_features()), *wkt, true, 0.0);
  CHECK_MESSAGE(diff.empty(), diff);
  CHECK(wkt->features[2].properties["label"].as_string() == "a, \"quoted\" label");
  const auto xy = test::read_as<ir::Geo>("geocsv", test::fixture("geo/csv/points_xy.csv"));
  REQUIRE_OK(xy);
  REQUIRE(xy->features.size() == 2);
  CHECK(geometry_diff(synthetic_features().features[0].geometry, xy->features[0].geometry).empty());
  CHECK(xy->features[0].properties["label"].as_string() == "Ünïcødé ✓ 東京");
}

TEST_CASE("geo/geocsv: a time column makes a track (GPS log / Strava-style export)") {
  const auto geo = test::read_as<ir::Geo>("geocsv", test::fixture("geo/csv/track_points_xy.csv"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 1);
  CHECK(geo->features[0].role == ir::FeatureRole::Track);
  const auto& pts = std::get<ir::MultiLineString>(geo->features[0].geometry.shape).lines[0].points;
  REQUIRE(pts.size() == 11);
  CHECK(pts[0] .time_ms == track_start_ms);
  CHECK(pts[7].time_ms == track_point(1, 1).time_ms);
  CHECK(pts[7].ele == track_point(1, 1).ele);
  CHECK_APPROX(pts[7].lon, track_point(1, 1).lon, 1e-9);
  // Forced feature-per-row reading.
  const auto rows = test::read_as<ir::Geo>("geocsv", test::fixture("geo/csv/track_points_xy.csv"), R"({"csv_rows":"features"})");
  REQUIRE_OK(rows);
  CHECK(rows->features.size() == 11);
}

TEST_CASE("geo/geocsv: write and read back, per feature and per point") {
  const auto bytes = test::write_from("geocsv", synthetic_features());
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("WKT,name,kind,rank"));
  const auto back = test::read_as<ir::Geo>("geocsv", *bytes);
  REQUIRE_OK(back);
  CHECK(features_diff(synthetic_features(), *back, true, 0.0).empty());
  ir::Geo points;
  points.features = {synthetic_features().features[0], synthetic_features().features[1]};
  const auto latlon = test::write_from("geocsv", points, R"({"csv_delimiter":"semicolon"})");
  REQUIRE_OK(latlon);
  CHECK(test::str(*latlon).starts_with("latitude;longitude;name"));
  const auto back_points = test::read_as<ir::Geo>("geocsv", *latlon);
  REQUIRE_OK(back_points);
  CHECK(features_diff(points, *back_points, true, 0.0).empty());
  ir::Geo track;
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.geometry.shape = synthetic_track();
  track.features.push_back(f);
  const auto per_point = test::write_from("geocsv", track, R"({"rows":"points"})");
  REQUIRE_OK(per_point);
  const std::string text = test::str(*per_point);
  CHECK(text.starts_with("feature,part,latitude,longitude,elevation,time,heart_rate,cadence,power,temperature\r\n"));
  CHECK(text.find("2024-03-10T08:05:10Z,131,85,210,17") != std::string::npos);
  const auto log = test::read_as<ir::Geo>("geocsv", *per_point);
  REQUIRE_OK(log);
  const auto& pts = std::get<ir::MultiLineString>(log->features[0].geometry.shape).lines[0].points;
  REQUIRE(pts.size() == 11);
  CHECK(pts[10] == track_point(1, 4));
  const auto both = test::write_from("geocsv", synthetic_features(), R"({"geometry_columns":"both"})");
  REQUIRE_OK(both);
  CHECK(test::str(*both).starts_with("WKT,latitude,longitude,"));
}

TEST_CASE("geo/geocsv: detection only with coordinate columns") {
  CHECK(test::convert(test::fixture("geo/csv/points_xy.csv"), "", "geojson", "", "points.csv").ok());
  CHECK(test::convert(test::fixture("geo/csv/features_wkt.csv"), "", "kml").ok());
  const test::Converted plain = test::convert("name,age\nAnn,3\n", "", "geojson", "", "people.csv");
  CHECK_FALSE(plain.ok());
  CHECK_STATUS(test::read_as<ir::Geo>("geocsv", test::bytes("name,age\nAnn,3\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("geocsv", test::bytes("WKT,a\n\"POINT (1 2\",3\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("geocsv", test::bytes("lat,lon\n\"1,2\n")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("geocsv", test::fixture("geo/csv/features_wkt.csv")) > 0);
}

// --------------------------------------------------------------------------- polylines

TEST_CASE("geo/polyline: decodes polylines from the python polyline package") {
  for (const char* format : {"polyline", "polyline6"}) {
    INFO(format);
    const std::string name = std::string("geo/polyline/lines.") + format;
    const auto geo = test::read_as<ir::Geo>(format, test::fixture(name));
    REQUIRE_OK(geo);
    REQUIRE(geo->features.size() == 2);
    const ir::Geo expected = synthetic_features();
    const double tolerance = std::string_view(format) == "polyline" ? 5e-6 : 5e-7;
    CHECK(geometry_diff(expected.features[2].geometry, geo->features[0].geometry, tolerance).empty());
    std::vector<ir::Coord> seg;
    for (int i = 0; i < 6; ++i) seg.push_back(track_point(0, i));
    CHECK(coords_diff(seg, std::get<ir::LineString>(geo->features[1].geometry.shape).points, tolerance).empty());
  }
}

TEST_CASE("geo/polyline: encoding matches the reference and round-trips") {
  const auto bytes = test::write_from("polyline", synthetic_features());
  REQUIRE_OK(bytes);
  const std::string reference = test::str(test::fixture("geo/polyline/lines.polyline"));
  // The first line of the reference is Charlie Path, the third feature.
  const std::string text = test::str(*bytes);
  CHECK(text.find(reference.substr(0, reference.find('\n') + 1)) != std::string::npos);
  const auto six = test::write_from("polyline6", synthetic_features());
  REQUIRE_OK(six);
  const std::string reference6 = test::str(test::fixture("geo/polyline/lines.polyline6"));
  CHECK(test::str(*six).find(reference6.substr(0, reference6.find('\n') + 1)) != std::string::npos);
  CHECK(test::convert(test::fixture("geo/polyline/lines.polyline6"), "", "geojson", "", "route.polyline6").ok());
  CHECK_STATUS(test::read_as<ir::Geo>("polyline", test::bytes("_p~iF~ps|U_ulLnnqC_mqNvxq`@ x")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("polyline", test::fixture("geo/polyline/lines.polyline")) > 0);
}

// --------------------------------------------------------------------------- geohash / Plus Codes

TEST_CASE("geo/geohash: decodes pygeohash codes to the cell centre") {
  const auto geo = test::read_as<ir::Geo>("geohash", test::fixture("geo/codes/points.geohash"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 3);
  const double lons[] = {-29.4812345, -29.45, -29.10};
  const double lats[] = {40.1234567, 40.15, 40.30};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto& c = std::get<ir::Point>(geo->features[i].geometry.shape).coord;
    CHECK_APPROX(c.lon, lons[i], geo->features[i].properties["cell_width"].as_double());
    CHECK_APPROX(c.lat, lats[i], geo->features[i].properties["cell_height"].as_double());
  }
  const auto encoded = test::write_from("geohash", synthetic_features());
  REQUIRE_OK(encoded);
  CHECK(test::str(*encoded).starts_with(test::str(test::fixture("geo/codes/points.geohash")).substr(0, 24)));
  const auto short_codes = test::write_from("geohash", *geo, R"({"code_length":6})");
  REQUIRE_OK(short_codes);
  CHECK(test::str(*short_codes) == test::str(test::fixture("geo/codes/points_short.geohash")));
}

TEST_CASE("geo/pluscode: decodes and encodes like openlocationcode") {
  const auto geo = test::read_as<ir::Geo>("pluscode", test::fixture("geo/codes/points.pluscode"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 3);
  const auto& c = std::get<ir::Point>(geo->features[0].geometry.shape).coord;
  CHECK_APPROX(c.lon, -29.4812345, geo->features[0].properties["cell_width"].as_double());
  CHECK_APPROX(c.lat, 40.1234567, geo->features[0].properties["cell_height"].as_double());
  ir::Geo points;
  for (auto [lon, lat] : {std::pair{-29.4812345, 40.1234567}, {-29.45, 40.15}, {-29.10, 40.30}}) {
    ir::Feature f;
    f.geometry.shape = ir::Point{ll(lon, lat)};
    points.features.push_back(f);
  }
  const auto encoded = test::write_from("pluscode", points);
  REQUIRE_OK(encoded);
  CHECK(test::str(*encoded) == test::str(test::fixture("geo/codes/points.pluscode")));
  CHECK_STATUS(test::read_as<ir::Geo>("pluscode", test::bytes("9G8F+6X\n")), Status::Decode);  // short code
  CHECK(test::convert(test::fixture("geo/codes/points.pluscode"), "", "geojson").ok());
  CHECK(test::fuzz_reader<ir::Geo>("pluscode", test::fixture("geo/codes/points.pluscode")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("geohash", test::fixture("geo/codes/points.geohash")) > 0);
}
