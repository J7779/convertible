// Tests of the GPS log formats: TCX, IGC and NMEA 0183.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

// All points of the first track, segments joined.
std::vector<ir::Coord> track_points(const ir::Geo& geo) {
  std::vector<ir::Coord> out;
  for (const auto& f : geo.features) {
    if (f.role != ir::FeatureRole::Track) continue;
    for (const auto& l : std::get<ir::MultiLineString>(f.geometry.shape).lines) {
      out.insert(out.end(), l.points.begin(), l.points.end());
    }
    break;
  }
  return out;
}

std::vector<ir::Coord> expected_points() {
  std::vector<ir::Coord> out;
  for (int s = 0; s < 2; ++s) {
    for (int i = 0; i < track_segment_sizes[s]; ++i) out.push_back(track_point(s, i));
  }
  return out;
}

ir::Geo synthetic_activity() {
  ir::Geo geo;
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.name = "Hotel Run";
  f.geometry.shape = synthetic_track();
  f.properties.set("sport", "running");
  geo.features.push_back(f);
  return geo;
}

}  // namespace

// --------------------------------------------------------------------------- TCX

TEST_CASE("geo/tcx: reads a GPSBabel activity with heart rate, cadence and speed") {
  const auto geo = test::read_as<ir::Geo>("tcx", test::fixture("geo/tcx/activity_gpsbabel.tcx"));
  REQUIRE_OK(geo);
  const auto pts = track_points(*geo);
  const auto want = expected_points();
  REQUIRE(pts.size() == want.size());
  CHECK(coords_diff(want, pts).empty());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(pts[i].time_ms == want[i].time_ms);
    CHECK(pts[i].ele == want[i].ele);
    CHECK(pts[i].heart_rate == want[i].heart_rate);
    CHECK(pts[i].cadence == want[i].cadence);
  }
  CHECK(geo->features[0].properties["sport"].as_string() == "Running");
  CHECK(geo->features[0].properties["laps"][0]["avg_heart_rate"] == Json(127.0));
}

TEST_CASE("geo/tcx: reads a GPSBabel course") {
  const auto geo = test::read_as<ir::Geo>("tcx", test::fixture("geo/tcx/course_gpsbabel.tcx"));
  REQUIRE_OK(geo);
  CHECK(geo->features[0].name == "Hotel Run");
  const auto pts = track_points(*geo);
  REQUIRE(pts.size() == 11);
  CHECK(coords_diff(expected_points(), pts).empty());
  CHECK(pts[1].speed == 4.799);
}

TEST_CASE("geo/tcx: activity and course round trips") {
  const auto bytes = test::write_from("tcx", synthetic_activity());
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).find("Sport=\"Running\"") != std::string::npos);
  const auto back = test::read_as<ir::Geo>("tcx", *bytes);
  REQUIRE_OK(back);
  const auto& lines = std::get<ir::MultiLineString>(back->features[0].geometry.shape).lines;
  REQUIRE(lines.size() == 2);
  ir::Coord expected = track_point(1, 2);
  expected.temperature.reset();  // TCX has no temperature
  CHECK(lines[1].points[2] == expected);
  CHECK(back->features[0].properties["laps"].size() == 2);
  std::vector<std::string> warnings;
  const auto course = test::write_from("tcx", synthetic_features(), "", &warnings);
  REQUIRE_OK(course);
  CHECK(test::str(*course).find("<Courses>") != std::string::npos);
  CHECK_FALSE(warnings.empty());
  CHECK(test::convert(test::fixture("geo/tcx/activity_gpsbabel.tcx"), "", "gpx").ok());
  CHECK(test::convert(test::fixture("geo/synthetic_track.gpx"), "", "tcx").ok());
}

TEST_CASE("geo/tcx: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Geo>("tcx", test::bytes("<TrainingCenterDatabase/>")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("tcx", test::bytes("<gpx/>")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("tcx", test::fixture("geo/tcx/activity_gpsbabel.tcx")) > 0);
}

// --------------------------------------------------------------------------- IGC

TEST_CASE("geo/igc: reads a GPSBabel IGC file") {
  const auto geo = test::read_as<ir::Geo>("igc", test::fixture("geo/igc/track_gpsbabel.igc"));
  REQUIRE_OK(geo);
  const auto pts = track_points(*geo);
  const auto want = expected_points();
  REQUIRE(pts.size() == want.size());
  // IGC stores thousandths of a minute: 1.7e-5 degrees.
  CHECK(coords_diff(want, pts, 1e-5).empty());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(pts[i].time_ms == want[i].time_ms);
    CHECK_APPROX(*pts[i].ele, *want[i].ele, 0.5);  // whole metres
  }
  CHECK(geo->metadata.contains("pltpilot"));
}

TEST_CASE("geo/igc: extensions, midnight roll-over and the new date record") {
  const std::string igc =
      "AXXX001\r\nHFDTEDATE:311223,01\r\nHFPLTPILOT:Nobody\r\nI023638FXA3940HRT\r\n"
      "B2359594012000N02927000WA001000012001599\r\nB0000014012001N02927001WA001000012002098\r\n";
  const auto geo = test::read_as<ir::Geo>("igc", test::bytes(igc));
  REQUIRE_OK(geo);
  const auto pts = track_points(*geo);
  REQUIRE(pts.size() == 2);
  CHECK(pts[0].time_ms == cvt::geo::parse_time("2023-12-31T23:59:59Z"));
  CHECK(pts[1].time_ms == cvt::geo::parse_time("2024-01-01T00:00:01Z"));
  CHECK(pts[0].heart_rate == 99.0);
  CHECK(pts[1].heart_rate == 98.0);
  CHECK(pts[0].ele == 120.0);
  const auto stripped = test::write_from("igc", *geo);
  REQUIRE_OK(stripped);
  CHECK(test::str(*stripped).find("Nobody") == std::string::npos);
}

TEST_CASE("geo/igc: write and read back") {
  const auto bytes = test::write_from("igc", synthetic_activity());
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("AXXX000\r\nHFDTEDATE:100324,01\r\nB0800004012000N02927000WA0000000100"));
  const auto back = test::read_as<ir::Geo>("igc", *bytes);
  REQUIRE_OK(back);
  CHECK(coords_diff(expected_points(), track_points(*back), 1e-5).empty());
  CHECK_STATUS(test::write_from("igc", synthetic_features()), Status::Encode);  // no times
  CHECK(test::convert(test::fixture("geo/igc/track_gpsbabel.igc"), "", "kml").ok());
  CHECK_STATUS(test::read_as<ir::Geo>("igc", test::bytes("AXXX\r\nB12\r\n")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("igc", test::fixture("geo/igc/track_gpsbabel.igc")) > 0);
}

// --------------------------------------------------------------------------- NMEA

TEST_CASE("geo/nmea: reads a GPSBabel NMEA log") {
  const auto geo = test::read_as<ir::Geo>("nmea", test::fixture("geo/nmea/track_gpsbabel.nmea"));
  REQUIRE_OK(geo);
  const auto pts = track_points(*geo);
  const auto want = expected_points();
  REQUIRE(pts.size() == want.size());
  CHECK(coords_diff(want, pts, 1e-6).empty());  // GPSBabel writes thousandths of a minute
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(pts[i].time_ms == want[i].time_ms);
    CHECK(pts[i].ele == want[i].ele);
  }
  bool waypoint = false;
  for (const auto& f : geo->features) waypoint = waypoint || f.role == ir::FeatureRole::Waypoint;
  CHECK(waypoint);
}

TEST_CASE("geo/nmea: checksums, talkers and round trips") {
  std::vector<std::string> warnings;
  const std::string log =
      "$GNRMC,235959.00,A,4012.000,N,02927.000,W,10.0,0.0,311223,,*FF\r\n"
      "$GNRMC,000001.00,A,4012.000,N,02927.000,W,10.0,0.0,010124,,*00\r\n"
      "$GNGGA,000001.00,4012.000,N,02927.000,W,1,08,0.9,55.5,M,0.0,M,,*6C\r\n";
  const auto geo = test::read_as<ir::Geo>("nmea", test::bytes(log), "", &warnings);
  REQUIRE_OK(geo);
  CHECK(warnings.size() == 1);  // the first sentence has a wrong checksum
  const auto pts = track_points(*geo);
  REQUIRE(pts.size() == 1);
  CHECK(pts[0].time_ms == cvt::geo::parse_time("2024-01-01T00:00:01Z"));
  CHECK(pts[0].ele == 55.5);
  CHECK_APPROX(*pts[0].speed, 10.0 * 1852 / 3600, 1e-9);
  const auto bytes = test::write_from("nmea", synthetic_activity());
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("nmea", *bytes);
  REQUIRE_OK(back);
  const auto back_pts = track_points(*back);
  CHECK(coords_diff(expected_points(), back_pts, 2e-7).empty());
  CHECK(back_pts[7].time_ms == track_point(1, 1).time_ms);
  CHECK(test::convert(test::fixture("geo/nmea/track_gpsbabel.nmea"), "", "gpx").ok());
  CHECK(test::fuzz_reader<ir::Geo>("nmea", test::fixture("geo/nmea/track_gpsbabel.nmea")) > 0);
}
