// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Playlists: M3U/M3U8 (incl. HLS), PLS, XSPF, ASX, WPL, iTunes library XML, CUE.
#include "engine/detect.hpp"
#include "ir/data.hpp"
#include "ir/table.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

/// The three entries every playlist fixture holds (see SOURCES.md).
void check_entries(const Json& data, bool titles, bool artists, bool durations) {
  const Json& entries = data["entries"];
  REQUIRE(entries.size() == 3);
  CHECK(entries[0]["location"].as_string() == "music/01 Überture.flac");
  CHECK(entries[1]["location"].as_string() == "music/02 שיר.mp3");
  CHECK(entries[2]["location"].as_string() == "https://radio.example.org/stream");
  if (titles) {
    CHECK(entries[0]["title"].as_string() == "Überture");
    CHECK(entries[1]["title"].as_string() == "שיר בדיוני");
    CHECK(entries[2]["title"].as_string() == "Example Radio 🎵");
  }
  if (artists) {
    CHECK(entries[0]["artist"].as_string() == "Ada Example");
    CHECK(entries[1]["artist"].as_string() == "Ben Sample");
  }
  if (durations) {
    CHECK_APPROX(entries[0]["duration_s"].as_double(), 183.5, 0.51);
    CHECK_APPROX(entries[1]["duration_s"].as_double(), 241.0, 1e-9);
  }
}

}  // namespace

TEST_CASE("pim/m3u8: decodes an extended M3U playlist") {
  const auto data = test::read_as<ir::Data>("m3u8", test::fixture("pim/playlists/extended.m3u8"));
  REQUIRE_OK(data);
  check_entries(data->value, true, true, true);
  CHECK(data->value["title"].as_string() == "Fixture mix");
  CHECK(data->value["entries"][0]["album"].as_string() == "Synthetic Sessions");
  CHECK(data->value["entries"][2]["group"].as_string() == "Live");
  CHECK(data->value["entries"][2]["attributes"]["tvg-id"].as_string() == "radio1");
  CHECK(!data->value["entries"][2].contains("duration_s"));
}

TEST_CASE("pim/m3u8: HLS media and master playlists written by the m3u8 library") {
  const auto media = test::read_as<ir::Data>("m3u8", test::fixture("pim/playlists/hls_media.m3u8"));
  REQUIRE_OK(media);
  const Json& m = media->value;
  CHECK(m["type"].as_string() == "hls-media");
  CHECK(m["target_duration"].as_int64() == 10);
  CHECK(m["media_sequence"].as_int64() == 7);
  CHECK(m["version"].as_int64() == 3);
  CHECK(m["endlist"].as_bool());
  REQUIRE(m["segments"].size() == 3);
  CHECK(m["segments"][0]["uri"].as_string() == "segment0.ts");
  CHECK_APPROX(m["segments"][0]["duration_s"].as_double(), 9.009, 1e-9);
  CHECK(m["segments"][2]["discontinuity"].as_bool());
  const auto master = test::read_as<ir::Data>("m3u8", test::fixture("pim/playlists/hls_master.m3u8"));
  REQUIRE_OK(master);
  CHECK(master->value["type"].as_string() == "hls-master");
  REQUIRE(master->value["variants"].size() == 2);
  CHECK(master->value["variants"][1]["bandwidth"].as_int64() == 2500000);
  CHECK(master->value["variants"][1]["codecs"].as_string() == "avc1.4d401f,mp4a.40.2");
  // m3u8 -> m3u8 keeps the HLS structure.
  for (const std::string_view file : {"pim/playlists/hls_media.m3u8", "pim/playlists/hls_master.m3u8"}) {
    INFO(file);
    const test::Converted again = test::convert(test::fixture(file), "m3u8", "m3u8");
    REQUIRE(again.ok());
    const auto original = test::read_as<ir::Data>("m3u8", test::fixture(file));
    const auto copy = test::read_as<ir::Data>("m3u8", again.data);
    REQUIRE_OK(copy);
    CHECK_MESSAGE(test::data_equal(original->value, copy->value), test::data_diff(original->value, copy->value));
  }
  // As a table: one row per segment.
  const auto table = test::read_as<ir::Table>("m3u8", test::fixture("pim/playlists/hls_media.m3u8"));
  REQUIRE_OK(table);
  CHECK(table->sheets[0].rows.size() == 4);
}

TEST_CASE("pim/playlists: PLS, XSPF, ASX, WPL decode files written by Python") {
  const auto pls = test::read_as<ir::Data>("pls", test::fixture("pim/playlists/configparser.pls"));
  REQUIRE_OK(pls);
  check_entries(pls->value, true, false, true);
  const auto xspf = test::read_as<ir::Data>("xspf", test::fixture("pim/playlists/etree.xspf"));
  REQUIRE_OK(xspf);
  check_entries(xspf->value, true, true, true);
  CHECK(xspf->value["title"].as_string() == "Fixture mix");
  CHECK(xspf->value["entries"][1]["track"].as_string() == "2");
  const auto asx = test::read_as<ir::Data>("asx", test::fixture("pim/playlists/etree.asx"));
  REQUIRE_OK(asx);
  check_entries(asx->value, true, true, true);
  const auto wpl = test::read_as<ir::Data>("wpl", test::fixture("pim/playlists/etree.wpl"));
  REQUIRE_OK(wpl);
  check_entries(wpl->value, false, false, false);
  CHECK(wpl->value["title"].as_string() == "Fixture mix");
}

TEST_CASE("pim/itunes-xml: reads a library written by plistlib") {
  const auto data = test::read_as<ir::Data>("itunes-xml", test::fixture("pim/playlists/itunes_library.xml"));
  REQUIRE_OK(data);
  REQUIRE(data->value["entries"].size() == 2);
  CHECK(data->value["entries"][0]["location"].as_string() == "/Music/01 Überture.flac");
  CHECK_APPROX(data->value["entries"][0]["duration_s"].as_double(), 183.5, 1e-9);
  const auto table = test::read_as<ir::Table>("itunes-xml", test::fixture("pim/playlists/itunes_library.xml"));
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 2);
  CHECK(table->sheets[1].name == "Favourites");
  CHECK(table->sheets[1].rows.size() == 2);
  CHECK(table->sheets[1].rows[1][1].text == "שיר בדיוני");
}

TEST_CASE("pim/cue: reads and writes CUE sheets") {
  const auto data = test::read_as<ir::Data>("cue", test::fixture("pim/playlists/album.cue"));
  REQUIRE_OK(data);
  const Json& cue = data->value;
  CHECK(cue["title"].as_string() == "Synthetic Sessions");
  CHECK(cue["rem"]["genre"].as_string() == "Ambient");
  const Json& tracks = cue["files"][0]["tracks"];
  REQUIRE(tracks.size() == 3);
  CHECK(tracks[1]["title"].as_string() == "Second movement");
  CHECK_APPROX(tracks[1]["indexes"]["01"].as_double(), 183.0 + 37.0 / 75.0, 1e-9);
  CHECK_APPROX(tracks[1]["indexes"]["00"].as_double(), 181.0 + 50.0 / 75.0, 1e-9);
  // cue -> cue is lossless for what the model holds.
  const test::Converted again = test::convert(test::fixture("pim/playlists/album.cue"), "cue", "cue");
  REQUIRE(again.ok());
  const auto copy = test::read_as<ir::Data>("cue", again.data);
  REQUIRE_OK(copy);
  CHECK_MESSAGE(test::data_equal(cue, copy->value, 1e-9), test::data_diff(cue, copy->value, 1e-9));
  // As a table: durations from the next track's start.
  const auto table = test::read_as<ir::Table>("cue", test::fixture("pim/playlists/album.cue"));
  REQUIRE_OK(table);
  CHECK_APPROX(table->sheets[0].rows[1][4].number, 183.0 + 37.0 / 75.0, 1e-9);
}

TEST_CASE("pim/playlists: every writable pair round-trips the entries") {
  const char* formats[] = {"m3u", "m3u8", "pls", "xspf", "asx", "wpl", "cue"};
  const auto source = test::read_as<ir::Table>("xspf", test::fixture("pim/playlists/etree.xspf"));
  REQUIRE_OK(source);
  for (const char* from : formats) {
    const auto written = test::write_from(from, *source);
    REQUIRE_OK(written);
    for (const char* to : formats) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*written, from, to);
      REQUIRE(converted.ok());
      const auto table = test::read_as<ir::Table>(to, converted.data);
      REQUIRE_OK(table);
      const ir::Sheet& sheet = table->sheets.at(0);
      // CUE sheets group tracks by file; every other format keeps three rows.
      REQUIRE(sheet.rows.size() == 4);
      for (std::size_t r = 1; r < 4; ++r) CHECK(sheet.rows[r][0].text == source->sheets[0].rows[r][0].text);
      const bool titles = std::string_view(from) != "wpl" && std::string_view(to) != "wpl";
      if (titles) CHECK(sheet.rows[1][1].text == "Überture");
    }
  }
}

TEST_CASE("pim/playlists: tables with other column names become playlists") {
  ir::Table table;
  ir::Sheet sheet;
  sheet.rows.push_back({ir::Cell::string("Path"), ir::Cell::string("Name"), ir::Cell::string("Length")});
  sheet.rows.push_back({ir::Cell::string("a.mp3"), ir::Cell::string("A"), ir::Cell::string("1:30")});
  table.sheets.push_back(sheet);
  const auto m3u = test::write_from("m3u", table);
  REQUIRE_OK(m3u);
  CHECK(test::str(*m3u) == "#EXTM3U\n#EXTINF:90,A\na.mp3\n");
  ir::Table bad;
  bad.sheets.push_back(ir::Sheet{"", {{ir::Cell::string("nothing")}}, true});
  CHECK_STATUS(test::write_from("m3u", bad), Status::Encode);
}

TEST_CASE("pim/playlists: detection and damaged input") {
  CHECK(detected("pim/playlists/extended.m3u8", "x.m3u8") == "m3u8");
  CHECK(detected("pim/playlists/extended.m3u8", "x.m3u") == "m3u");
  CHECK(detected("pim/playlists/hls_media.m3u8", "") == "m3u8");
  CHECK(detected("pim/playlists/configparser.pls", "") == "pls");
  CHECK(detected("pim/playlists/etree.xspf", "") == "xspf");
  CHECK(detected("pim/playlists/etree.asx", "") == "asx");
  CHECK(detected("pim/playlists/etree.wpl", "") == "wpl");
  CHECK(detected("pim/playlists/itunes_library.xml", "Library.xml") == "itunes-xml");
  CHECK(detected("pim/playlists/album.cue", "") == "cue");
  struct Case {
    const char* format;
    const char* file;
  };
  const Case cases[] = {{"m3u8", "pim/playlists/extended.m3u8"}, {"m3u8", "pim/playlists/hls_media.m3u8"},
                        {"pls", "pim/playlists/configparser.pls"}, {"xspf", "pim/playlists/etree.xspf"},
                        {"asx", "pim/playlists/etree.asx"},        {"wpl", "pim/playlists/etree.wpl"},
                        {"itunes-xml", "pim/playlists/itunes_library.xml"}, {"cue", "pim/playlists/album.cue"}};
  for (const Case& c : cases) {
    INFO(c.format);
    CHECK(test::fuzz_reader<ir::Data>(c.format, test::fixture(c.file)) > 0);
    CHECK(test::fuzz_reader<ir::Table>(c.format, test::fixture(c.file), 64) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("xspf", test::bytes("<playlist")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Data>("cue", test::bytes("garbage\n")), Status::Decode);
}
