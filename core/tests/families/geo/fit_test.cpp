// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

std::uint16_t crc16(ByteView data) {
  static constexpr std::uint16_t table[16] = {0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                              0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};
  std::uint16_t crc = 0;
  for (std::byte b : data) {
    const auto byte = std::to_integer<std::uint8_t>(b);
    std::uint16_t tmp = table[crc & 0xF];
    crc = static_cast<std::uint16_t>(((crc >> 4) & 0x0FFF) ^ tmp ^ table[byte & 0xF]);
    tmp = table[crc & 0xF];
    crc = static_cast<std::uint16_t>(((crc >> 4) & 0x0FFF) ^ tmp ^ table[(byte >> 4) & 0xF]);
  }
  return crc;
}

// Wraps FIT messages in a 12-byte header and a file CRC.
Bytes fit_file(const Bytes& body) {
  util::ByteWriter w;
  w.u8(12).u8(0x20).u16le(2100).u32le(static_cast<std::uint32_t>(body.size())).text(".FIT").bytes(body);
  w.u16le(crc16(w.view()));
  return w.take();
}

const ir::Feature& track_of(const ir::Geo& geo) {
  for (const auto& f : geo.features) {
    if (f.role == ir::FeatureRole::Track) return f;
  }
  return geo.features.front();
}

}  // namespace

TEST_CASE("geo/fit: decodes an activity written by fit-tool exactly as fitparse does") {
  const auto geo = test::read_as<ir::Geo>("fit", test::fixture("geo/fit/activity_fittool.fit"));
  REQUIRE_OK(geo);
  const ir::Feature& f = track_of(*geo);
  REQUIRE(f.role == ir::FeatureRole::Track);
  const auto& track = std::get<ir::MultiLineString>(f.geometry.shape);
  REQUIRE(track.lines.size() == 2);  // one segment per lap
  CHECK(track.lines[0].points.size() == 6);
  CHECK(track.lines[1].points.size() == 5);
  // Reference values decoded by fitparse (an independent decoder).
  const auto reference = util::parse_json(test::fixture_text("geo/fit/activity_fittool.records.json"));
  REQUIRE_OK(reference);
  REQUIRE(reference->size() == 11);
  std::size_t i = 0;
  for (const auto& line : track.lines) {
    for (const ir::Coord& c : line.points) {
      const Json& r = (*reference)[i];
      INFO("record ", i);
      CHECK(c.lat == r["position_lat"].as_double() * 180.0 / 2147483648.0);
      CHECK(c.lon == r["position_long"].as_double() * 180.0 / 2147483648.0);
      CHECK_APPROX(*c.ele, r["altitude"].as_double(), 1e-9);
      CHECK(c.heart_rate == r["heart_rate"].as_double());
      CHECK(c.cadence == r["cadence"].as_double());
      CHECK(c.power == r["power"].as_double());
      CHECK(c.temperature == r["temperature"].as_double());
      CHECK(c.time_ms == cvt::geo::parse_time(r["timestamp"].as_string() + "Z"));
      ++i;
    }
  }
  // And against the synthetic track: semicircles quantise to ~8e-8 degrees, altitude to 0.2 m.
  const ir::MultiLineString expected = synthetic_track();
  for (int s = 0; s < 2; ++s) {
    CHECK(coords_diff(expected.lines[s].points, track.lines[s].points, 1e-7).empty());
  }
  CHECK(f.properties["sport"].as_string() == "running");
  CHECK(f.properties["laps"].size() == 2);
  CHECK(f.properties["session"]["total_elapsed_time_s"] == Json(340.0));
  CHECK(geo->metadata["manufacturer"] == Json(255));
}

TEST_CASE("geo/fit: decodes a course written by GPSBabel") {
  const auto geo = test::read_as<ir::Geo>("fit", test::fixture("geo/fit/course_gpsbabel.fit"));
  REQUIRE_OK(geo);
  const auto& track = std::get<ir::MultiLineString>(track_of(*geo).geometry.shape);
  std::size_t points = 0;
  for (const auto& l : track.lines) points += l.points.size();
  CHECK(points == 11);
  CHECK(std::fabs(track.lines[0].points[0].lat - 40.2) < 1e-7);
}

TEST_CASE("geo/fit: developer fields, compressed timestamps and big-endian definitions") {
  util::ByteWriter b;
  // developer_data_id (207): developer_data_index = 0
  b.u8(0x40).u8(0).u8(0).u16le(207).u8(1).u8(3).u8(1).u8(0x02);
  b.u8(0x00).u8(0);
  // field_description (206): index 0, field 0, uint16, name "doughnuts"
  b.u8(0x41).u8(0).u8(0).u16le(206).u8(4).u8(0).u8(1).u8(0x02).u8(1).u8(1).u8(0x02).u8(2).u8(1).u8(0x02).u8(3).u8(10).u8(0x07);
  b.u8(0x01).u8(0).u8(0).u8(0x84).text(std::string_view("doughnuts\0", 10));
  // record definition, big endian, with the developer field: timestamp, lat, lon
  b.u8(0x62).u8(0).u8(1).u16be(20).u8(3).u8(253).u8(4).u8(0x86).u8(0).u8(4).u8(0x85).u8(1).u8(4).u8(0x85);
  b.u8(1).u8(0).u8(2).u8(0);
  const std::uint32_t t0 = 1078992000;  // 2024-03-10T08:00:00Z in FIT seconds
  b.u8(0x02).u32be(t0).i32be(479604681).i32be(-351352186).u16be(7);
  // compressed-timestamp record (local type 3, 5 s later) without timestamp field
  b.u8(0x43).u8(0).u8(0).u16le(20).u8(2).u8(0).u8(4).u8(0x85).u8(1).u8(4).u8(0x85);
  const std::uint32_t offset = (t0 + 5) & 0x1F;
  b.u8(static_cast<std::uint8_t>(0x80 | (3 << 5) | offset)).i32le(479607067).i32le(-351346221);
  const Bytes file = fit_file(b.take());
  const auto geo = test::read_as<ir::Geo>("fit", file);
  REQUIRE_OK(geo);
  const auto& pts = std::get<ir::MultiLineString>(geo->features[0].geometry.shape).lines[0].points;
  REQUIRE(pts.size() == 2);
  CHECK(pts[0].time_ms == track_start_ms);
  CHECK(pts[1].time_ms == track_start_ms + 5000);
  REQUIRE(pts[0].extra.size() == 1);
  CHECK(pts[0].extra[0].first == "doughnuts");
  CHECK(pts[0].extra[0].second == 7.0);
  CHECK_APPROX(pts[1].lat, 40.2002, 1e-7);
}

TEST_CASE("geo/fit: write and read back an activity") {
  ir::Geo geo;
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.geometry.shape = synthetic_track();
  f.properties.set("sport", "cycling");
  geo.features.push_back(f);
  geo.metadata.set("serial_number", 1234);
  const auto bytes = test::write_from("fit", geo);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("fit", *bytes);
  REQUIRE_OK(back);
  const auto& track = std::get<ir::MultiLineString>(back->features[0].geometry.shape);
  REQUIRE(track.lines.size() == 2);
  const ir::MultiLineString expected = synthetic_track();
  for (int s = 0; s < 2; ++s) {
    CHECK(coords_diff(expected.lines[s].points, track.lines[s].points, 1e-7).empty());
    for (std::size_t i = 0; i < track.lines[s].points.size(); ++i) {
      const ir::Coord& got = track.lines[s].points[i];
      const ir::Coord& want = expected.lines[s].points[i];
      CHECK(got.time_ms == want.time_ms);
      CHECK_APPROX(*got.ele, *want.ele, 0.2);
      CHECK(got.heart_rate == want.heart_rate);
      CHECK(got.cadence == want.cadence);
      CHECK(got.power == want.power);
      CHECK(got.temperature == want.temperature);
    }
  }
  CHECK(back->features[0].properties["sport"].as_string() == "cycling");
  CHECK(back->features[0].properties["laps"].size() == 2);
  CHECK_FALSE(back->metadata.contains("serial_number"));  // no serial written
  // Dropping sensor data removes the record fields.
  const auto bare = test::write_from("fit", geo, R"({"keep_sensors":false})");
  REQUIRE_OK(bare);
  CHECK(bare->size() < bytes->size());
  const auto bare_back = test::read_as<ir::Geo>("fit", *bare);
  REQUIRE_OK(bare_back);
  CHECK_FALSE(std::get<ir::MultiLineString>(bare_back->features[0].geometry.shape).lines[0].points[0].heart_rate);
  // Lines without time still give a valid file, with a note.
  std::vector<std::string> warnings;
  const auto untimed = test::write_from("fit", synthetic_features(), "", &warnings);
  REQUIRE_OK(untimed);
  CHECK_FALSE(warnings.empty());
  CHECK_OK(test::read_as<ir::Geo>("fit", *untimed));
}

TEST_CASE("geo/fit: detection, conversion and metadata stripping") {
  const Bytes fit = test::fixture("geo/fit/activity_fittool.fit");
  const test::Converted gpx = test::convert(fit, "", "gpx");
  REQUIRE(gpx.ok());
  CHECK(gpx.text().find("<gpxtpx:hr>125</gpxtpx:hr>") != std::string::npos);
  CHECK(test::convert(fit, "", "fit").ok());
  CHECK(test::convert(test::fixture("geo/synthetic_track.gpx"), "gpx", "fit").ok());
  CHECK(test::convert(fit, "fit", "tcx").ok());
}

TEST_CASE("geo/fit: damaged files fail cleanly") {
  Bytes fit = test::fixture("geo/fit/activity_fittool.fit");
  CHECK_STATUS(test::read_as<ir::Geo>("fit", ByteView(fit).first(fit.size() - 1)), Status::Decode);
  Bytes corrupt = fit;
  corrupt[40] ^= std::byte{0x01};
  CHECK_STATUS(test::read_as<ir::Geo>("fit", corrupt), Status::Decode);  // CRC mismatch
  // An undefined local message type.
  const Bytes undefined = fit_file(test::bytes(std::string_view("\x05\x00\x00", 3)));
  CHECK_STATUS(test::read_as<ir::Geo>("fit", undefined), Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("fit", fit) > 0);
}
