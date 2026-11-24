// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

const ir::Feature* by_role(const ir::Geo& geo, ir::FeatureRole role) {
  for (const auto& f : geo.features) {
    if (f.role == role) return &f;
  }
  return nullptr;
}

// Checks positions, elevation, time and (optionally) sensors of the "Hotel Run" track.
void check_track(const ir::Feature& f, bool sensors, bool power = true) {
  const auto* track = std::get_if<ir::MultiLineString>(&f.geometry.shape);
  REQUIRE(track != nullptr);
  const ir::MultiLineString expected = synthetic_track();
  REQUIRE(track->lines.size() == 2);
  for (int s = 0; s < 2; ++s) {
    CHECK_MESSAGE(coords_diff(expected.lines[s].points, track->lines[s].points).empty(),
                  coords_diff(expected.lines[s].points, track->lines[s].points));
    for (std::size_t i = 0; i < track->lines[s].points.size() && i < expected.lines[s].points.size(); ++i) {
      const ir::Coord& want = expected.lines[s].points[i];
      const ir::Coord& got = track->lines[s].points[i];
      CHECK(got.time_ms == want.time_ms);
      REQUIRE(got.ele.has_value());
      CHECK_APPROX(*got.ele, *want.ele, 1e-6);
      if (sensors) {
        CHECK(got.heart_rate == want.heart_rate);
        CHECK(got.cadence == want.cadence);
        CHECK(got.temperature == want.temperature);
        if (power) CHECK(got.power == want.power);
      }
    }
  }
}

}  // namespace

TEST_CASE("geo/gpx: reads the synthetic GPX 1.1 with Garmin extensions") {
  const auto geo = test::read_as<ir::Geo>("gpx", test::fixture("geo/synthetic_track.gpx"));
  REQUIRE_OK(geo);
  CHECK(geo->name == "Hotel Outing");
  const ir::Feature* wpt = by_role(*geo, ir::FeatureRole::Waypoint);
  REQUIRE(wpt != nullptr);
  CHECK(wpt->name == "India Camp");
  CHECK(wpt->description == "base camp");
  const auto& p = std::get<ir::Point>(wpt->geometry.shape).coord;
  CHECK_APPROX(p.lon, waypoint_lon, 1e-9);
  CHECK_APPROX(p.lat, waypoint_lat, 1e-9);
  CHECK(p.time_ms == waypoint_time_ms);
  const ir::Feature* rte = by_role(*geo, ir::FeatureRole::Route);
  REQUIRE(rte != nullptr);
  CHECK(coords_diff(synthetic_route(), std::get<ir::LineString>(rte->geometry.shape).points).empty());
  const ir::Feature* trk = by_role(*geo, ir::FeatureRole::Track);
  REQUIRE(trk != nullptr);
  CHECK(trk->name == "Hotel Run");
  check_track(*trk, true);
}

TEST_CASE("geo/gpx: reads GPSBabel GPX 1.0 and 1.1 output") {
  const auto v10 = test::read_as<ir::Geo>("gpx", test::fixture("geo/gpx/track_gpsbabel_10.gpx"));
  REQUIRE_OK(v10);
  REQUIRE(by_role(*v10, ir::FeatureRole::Track) != nullptr);
  check_track(*by_role(*v10, ir::FeatureRole::Track), false);
  CHECK(v10->metadata["creator"].as_string().starts_with("GPSBabel"));
  const auto v11 = test::read_as<ir::Geo>("gpx", test::fixture("geo/gpx/track_gpsbabel_11.gpx"));
  REQUIRE_OK(v11);
  REQUIRE(by_role(*v11, ir::FeatureRole::Track) != nullptr);
  check_track(*by_role(*v11, ir::FeatureRole::Track), true, false);  // GPSBabel drops power
  const ir::Feature* rte = by_role(*v11, ir::FeatureRole::Route);
  REQUIRE(rte != nullptr);
  CHECK(coords_diff(synthetic_route(), std::get<ir::LineString>(rte->geometry.shape).points).empty());
}

TEST_CASE("geo/gpx: reads OGR waypoints with extension attributes") {
  const auto geo = test::read_as<ir::Geo>("gpx", test::fixture("geo/gpx/points_ogr.gpx"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 2);
  const ir::Geo expected = synthetic_features();
  CHECK(geometry_diff(expected.features[0].geometry, geo->features[0].geometry).empty());
  CHECK(geo->features[0].name == "Alpha Beacon");
  CHECK(geo->features[0].properties["label"].as_string() == "Ünïcødé ✓ 東京");
  CHECK(geo->features[0].properties["rank"] == Json(3));
  CHECK(geo->features[1].properties["height"] == Json(0.25));
}

TEST_CASE("geo/gpx: write and read back keeps tracks, times and sensors") {
  const auto source = test::read_as<ir::Geo>("gpx", test::fixture("geo/synthetic_track.gpx"));
  REQUIRE_OK(source);
  const auto bytes = test::write_from("gpx", *source);
  REQUIRE_OK(bytes);
  const std::string text = test::str(*bytes);
  CHECK(text.find("creator=\"\"") != std::string::npos);  // strip_metadata: no creator
  CHECK(text.find("<wpt") < text.find("<rte"));
  CHECK(text.find("<rte") < text.find("<trk>"));
  const auto back = test::read_as<ir::Geo>("gpx", *bytes);
  REQUIRE_OK(back);
  REQUIRE(back->features.size() == 3);
  check_track(*by_role(*back, ir::FeatureRole::Track), true);
  CHECK(by_role(*back, ir::FeatureRole::Waypoint)->name == "India Camp");
}

TEST_CASE("geo/gpx: options drop time and sensors, merge or split tracks, keep metadata") {
  const Bytes gpx = test::fixture("geo/synthetic_track.gpx");
  const auto source = test::read_as<ir::Geo>("gpx", gpx);
  REQUIRE_OK(source);
  const auto plain = test::write_from("gpx", *source, R"({"keep_time":false,"keep_sensors":false})");
  REQUIRE_OK(plain);
  CHECK(test::str(*plain).find("<time>") == std::string::npos);
  CHECK(test::str(*plain).find("gpxtpx:hr") == std::string::npos);
  const auto merged = test::read_as<ir::Geo>("gpx", *test::write_from("gpx", *source, R"({"track_mode":"merge"})"));
  REQUIRE_OK(merged);
  const auto& merged_track = std::get<ir::MultiLineString>(by_role(*merged, ir::FeatureRole::Track)->geometry.shape);
  CHECK(merged_track.lines.size() == 1);
  CHECK(merged_track.lines[0].points.size() == 11);
  const auto split = test::read_as<ir::Geo>("gpx", *test::write_from("gpx", *source, R"({"track_mode":"split"})"));
  REQUIRE_OK(split);
  CHECK(split->features.size() == 4);
  const auto kept = test::write_from("gpx", *source, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(test::str(*kept).find("creator=\"synthetic\"") != std::string::npos);
  const auto rounded = test::write_from("gpx", *source, R"({"precision":3})");
  REQUIRE_OK(rounded);
  CHECK(test::str(*rounded).find("lat=\"40.2\" lon=\"-29.45\"") != std::string::npos);
  CHECK(test::str(*rounded).find("-29.4495") == std::string::npos);
}

TEST_CASE("geo/gpx: polygons become track segments with a warning") {
  std::vector<std::string> warnings;
  const auto bytes = test::write_from("gpx", synthetic_features(), "", &warnings);
  REQUIRE_OK(bytes);
  CHECK(warnings.size() == 2);
  const auto back = test::read_as<ir::Geo>("gpx", *bytes);
  REQUIRE_OK(back);
  CHECK(back->features.size() == 2 + 1 + 1 + 1 + 1 + 2);  // points, line, polygon, multipolygon, lines, multipoint
}

TEST_CASE("geo/gpx: detection and conversion") {
  const Bytes gpx = test::fixture("geo/synthetic_track.gpx");
  const test::Converted converted = test::convert(gpx, "", "geojson", "", "");
  CHECK(converted.ok());
  CHECK(converted.text().find("\"_gpxType\":\"trk\"") != std::string::npos);
  CHECK(test::convert(gpx, "", "gpx", "", "track.gpx").ok());
}

TEST_CASE("geo/gpx: damaged input fails cleanly") {
  const Bytes gpx = test::fixture("geo/synthetic_track.gpx");
  CHECK_STATUS(test::read_as<ir::Geo>("gpx", test::bytes("<gpx><wpt lat=\"x\" lon=\"1\"/></gpx>")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("gpx", test::bytes("<kml/>")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("gpx", test::bytes("<gpx><trk>")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("gpx", gpx) > 0);
}
