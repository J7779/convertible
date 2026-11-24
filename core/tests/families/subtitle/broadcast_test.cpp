// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// TTML / DFXP / EBU-TT / SMPTE-TT, SAMI, YouTube srv3 and json3, Scenarist
// SCC and EBU STL: decoding of real files, round trips and robustness.
#include "engine/detect.hpp"
#include "families/subtitle/subtitle_test_support.hpp"

using namespace cvt;
using namespace cvt::test::subs;

namespace {

/// The source script of the pycaption fixtures (see SOURCES.md).
const std::vector<Expected>& pycaption_script() {
  static const std::vector<Expected> script = {
      {1000, 3500, "Hello, Ada Example."},
      {4000, 6250, "<i>Café déjà vu</i>\nsecond line"},
      {7000, 9000, "Plain caption text"},
      {10000, 12500, "Ünïcödé and ½ ♪ ©"},
  };
  return script;
}

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

}  // namespace

TEST_CASE("subtitle/dfxp: decodes a file written by pycaption") {
  const auto subs = test::read_as<ir::Subtitles>("dfxp", test::fixture("subtitle/pycaption.dfxp"));
  REQUIRE_OK(subs);
  // pycaption escapes the SRT italics as text; they read back as markup.
  check_cues(*subs, pycaption_script(), 0, false);
  CHECK(subs->metadata.at("language") == "en");
}

TEST_CASE("subtitle/ttml: every time expression, style references and regions") {
  const auto subs = test::read_as<ir::Subtitles>("ttml", test::fixture("subtitle/timing_forms.ttml"));
  REQUIRE_OK(subs);
  CHECK(subs->metadata.at("title") == "Timing forms");
  CHECK(subs->metadata.at("language") == "fr");
  check_cues(*subs,
             {
                 {1000, 3500, "Clock time, relative to the div"},
                 {4500, 6000, "Offset <i>seconds</i> and <b>milliseconds</b>"},
                 // 100 frames at 25 fps = 4 s after the div's 1 s; 0.05 h = 180 s
                 {5000, 181000, "<i>Frames offset, hours offset</i>"},
                 // 6 s 10 frames + 1 s from the div; 7 s 12.1 frames (sub-frame rate 2)
                 {7400, 8500, "Frames and sub-frames"},
                 {20000, 22000, "Ticks\ntwo lines"},
                 {25000, 26000, "<u>Under</u>lined"},
             },
             0, false, true);
  // Region "low" is bottom left; "top" is a top region.
  CHECK(subs->cues[0].position.value_or(ir::CuePosition{}).alignment == 1);
  CHECK(subs->cues[0].style.value_or("") == "low");
  CHECK(subs->cues[3].position.value_or(ir::CuePosition{}).alignment == 8);  // region "top", centred
  CHECK(subs->cues[3].style.value_or("") == "top");
}

TEST_CASE("subtitle/ebu-tt and smpte-tt: decode hand-written documents") {
  const auto ebu = test::read_as<ir::Subtitles>("ebu-tt", test::fixture("subtitle/ebu_tt_d.xml"));
  REQUIRE_OK(ebu);
  check_cues(*ebu, {{1000, 3000, "Grüße aus Köln"}, {4000, 6000, "<i>Kursiv</i>\nzweite Zeile"}}, 0, false);
  CHECK(ebu->metadata.at("language") == "de");
  const auto smpte = test::read_as<ir::Subtitles>("smpte-tt", test::fixture("subtitle/smpte_tt.xml"));
  REQUIRE_OK(smpte);
  // Frames at 30000/1001 fps: frame 15 = 500.5 ms.
  check_cues(*smpte, {{1000, 3000, "SMPTE frames at 29.97"}, {4501, 6000, "Second caption"}}, 1, false);
}

TEST_CASE("subtitle/sami: decodes a file written by pycaption and picks languages") {
  const auto subs = test::read_as<ir::Subtitles>("smi", test::fixture("subtitle/pycaption.smi"));
  REQUIRE_OK(subs);
  check_cues(*subs, pycaption_script(), 0, false);
  CHECK(subs->metadata.at("language") == "en");
  const std::string_view two_languages =
      "<SAMI><HEAD><STYLE TYPE=\"text/css\"><!--\n.ENUSCC { Name: English; lang: en-US; }\n"
      ".FRFRCC { Name: French; lang: fr-FR; }\n--></STYLE></HEAD><BODY>\n"
      "<SYNC Start=1000><P Class=ENUSCC>Hello<P Class=FRFRCC>Bonjour\n"
      "<SYNC Start=2000><P Class=ENUSCC>&nbsp;<P Class=FRFRCC>&nbsp;\n</BODY></SAMI>";
  const auto english = test::read_as<ir::Subtitles>("smi", test::bytes(two_languages));
  REQUIRE_OK(english);
  REQUIRE(english->cues.size() == 1);
  CHECK(english->cues[0].text == "Hello");
  const auto french = test::read_as<ir::Subtitles>("smi", test::bytes(two_languages), R"({"sami_language":"fr"})");
  REQUIRE_OK(french);
  REQUIRE(french->cues.size() == 1);
  CHECK(french->cues[0].text == "Bonjour");
  CHECK(french->cues[0].end_ms == 2000);
  CHECK_STATUS(test::read_as<ir::Subtitles>("smi", test::bytes(two_languages), R"({"sami_language":"de"})"),
               Status::InvalidArgument);
}

TEST_CASE("subtitle/srv3 and json3: decode YouTube caption files") {
  const auto srv3 = test::read_as<ir::Subtitles>("srv3", test::fixture("subtitle/youtube.srv3"));
  REQUIRE_OK(srv3);
  check_cues(*srv3, {{1000, 3500, "Hello, Ada Example."}, {4000, 6250, "Café & déjà vu\nsecond line"}}, 0, false);
  const auto json3 = test::read_as<ir::Subtitles>("json3", test::fixture("subtitle/youtube.json3"));
  REQUIRE_OK(json3);
  check_cues(*json3, {{1000, 3500, "Hello, Ada Example."}, {4000, 6250, "Café déjà vu\nsecond line"}}, 0, false);
}

TEST_CASE("subtitle/scc: decodes pop-on captions written by pycaption") {
  const auto subs = test::read_as<ir::Subtitles>("scc", test::fixture("subtitle/pycaption.scc"));
  REQUIRE_OK(subs);
  // pycaption shows each caption up to three frames early or late: compare
  // starts within 100 ms and texts exactly (special, extended characters, italics).
  REQUIRE(subs->cues.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    INFO("cue ", i);
    CHECK(std::llabs(subs->cues[i].start_ms - pycaption_script()[i].start_ms) <= 100);
    CHECK(subs->cues[i].text == pycaption_script()[i].text);
  }
}

TEST_CASE("subtitle/scc: roll-up and paint-on captions") {
  const auto subs = test::read_as<ir::Subtitles>("scc", test::fixture("subtitle/rollup_painton.scc"));
  REQUIRE_OK(subs);
  REQUIRE(subs->cues.size() == 4);
  // Drop-frame time codes ;00 -> frame counts at 29.97 fps.
  CHECK(std::llabs(subs->cues[0].start_ms - 1000) <= 70);
  CHECK(subs->cues[0].text == "FIRST ROLL-UP LINE");
  CHECK(subs->cues[1].text == "FIRST ROLL-UP LINE\nSECOND LINE");  // two-row roll-up window
  CHECK(subs->cues[2].text == "SECOND LINE\nTHIRD LINE");
  CHECK(subs->cues[3].text == "<i>PAINT ON</i>");
  CHECK(std::llabs(subs->cues[3].end_ms - 10000) <= 70);
}

TEST_CASE("subtitle/ebu-stl: decodes a hand-made Tech 3264 file") {
  const auto subs = test::read_as<ir::Subtitles>("ebu-stl", test::fixture("subtitle/handmade.stl"));
  REQUIRE_OK(subs);
  CHECK(subs->metadata.at("title") == "Fixture programme");
  CHECK(subs->metadata.at("language") == "en");
  // Start of programme 10:00:00:00 is subtracted; the comment block is skipped.
  check_cues(*subs,
             {{1000, 3480, "Hello, Ada Example."},
              {4000, 6240, "<i>Café déjà vu</i>\nsecond line"},
              {7000, 9000, "Top left ♪ ½ ©"},
              {10000, 12000, "<u>Grüße</u> aus Köln"}},
             0, false);
  CHECK(subs->cues[2].position.value_or(ir::CuePosition{}).alignment == 7);
  CHECK(subs->cues[3].position.value_or(ir::CuePosition{}).alignment == 3);
  const auto absolute = test::read_as<ir::Subtitles>("ebu-stl", test::fixture("subtitle/handmade.stl"),
                                                     R"({"stl_subtract_tcp":false})");
  REQUIRE_OK(absolute);
  CHECK(absolute->cues[0].start_ms == 36001000);
}

TEST_CASE("subtitle: broadcast and XML formats round-trip the sample track") {
  struct Format {
    std::string_view id;
    std::int64_t tolerance;
    bool markup;
    std::string_view options;
  };
  const Format formats[] = {
      {"ttml", 0, true, ""},   {"ttml", 0, true, R"({"ttml_profile":"imsc1","ttml_time_format":"offset"})"},
      {"dfxp", 0, true, ""},   {"ebu-tt", 0, true, ""},
      {"smi", 0, true, ""},   {"srv3", 0, false, ""},
      {"json3", 0, false, ""}, {"scc", 34, true, ""},
      {"ebu-stl", 20, false, ""}, {"ebu-stl", 17, false, R"({"stl_fps":30})"},
  };
  ir::Subtitles track = sample_track();
  for (const Format& format : formats) {
    INFO(format.id, " ", format.options);
    std::vector<std::string> warnings;
    const auto encoded = test::write_from(format.id, track, format.options, &warnings);
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Subtitles>(format.id, *encoded);
    REQUIRE_OK(decoded);
    if (format.id == "scc") {
      // CEA-608 has no Greek or Hebrew: those characters become '?'.
      REQUIRE(decoded->cues.size() == track.cues.size());
      for (std::size_t i = 0; i < track.cues.size(); ++i) {
        INFO("cue ", i);
        CHECK(std::llabs(decoded->cues[i].start_ms - track.cues[i].start_ms) <= format.tolerance);
        CHECK(std::llabs(decoded->cues[i].end_ms - track.cues[i].end_ms) <= format.tolerance);
        if (i != 2) CHECK(subtitle::strip_markup(decoded->cues[i].text) == subtitle::strip_markup(track.cues[i].text));
      }
      CHECK(decoded->cues[1].text == "<i>Two lines</i>\nwith ünïcödé");
      CHECK(!warnings.empty());
      continue;
    }
    if (format.id == "ebu-stl") {
      // ISO 6937 has Greek letters only as '?', and no bold.
      REQUIRE(decoded->cues.size() == track.cues.size());
      CHECK(decoded->cues[1].text == "<i>Two lines</i>\nwith ünïcödé");
      CHECK(decoded->cues[3].text == "Bold & <u>under</u>");
      for (std::size_t i = 0; i < track.cues.size(); ++i) {
        CHECK(std::llabs(decoded->cues[i].start_ms - track.cues[i].start_ms) <= format.tolerance);
      }
      continue;
    }
    check_cues(*decoded, expected_of(track), format.tolerance, !format.markup);
  }
}

TEST_CASE("subtitle/scc: written captions are pop-on with EOC at the cue start") {
  ir::Subtitles track;
  ir::Cue cue;
  cue.start_ms = 2000;
  cue.end_ms = 4000;
  cue.text = "Hi";
  track.cues.push_back(cue);
  const auto scc = test::write_from("scc", track);
  REQUIRE_OK(scc);
  const std::string text = test::str(*scc);
  CHECK(text.starts_with("Scenarist_SCC V1.0\r\n"));
  CHECK(text.find("00:00:02;00\t942f 942f") != std::string::npos);  // EOC at 2 s = frame 60
  CHECK(text.find("00:00:04;00\t942c 942c") != std::string::npos);  // EDM at 4 s = frame 120
}

TEST_CASE("subtitle: broadcast and XML formats are detected; damaged input is an error") {
  CHECK(detected("subtitle/pycaption.dfxp", "x.dfxp") == "dfxp");
  CHECK(detected("subtitle/timing_forms.ttml", "x.ttml") == "ttml");
  CHECK(detected("subtitle/timing_forms.ttml", "x.xml") == "ttml");
  CHECK(detected("subtitle/ebu_tt_d.xml", "x.xml") == "ebu-tt");
  CHECK(detected("subtitle/smpte_tt.xml", "x.xml") == "smpte-tt");
  CHECK(detected("subtitle/pycaption.smi", "") == "smi");
  CHECK(detected("subtitle/youtube.srv3", "") == "srv3");
  CHECK(detected("subtitle/youtube.json3", "x.json") == "json3");
  CHECK(detected("subtitle/pycaption.scc", "") == "scc");
  CHECK(detected("subtitle/handmade.stl", "x.stl") == "ebu-stl");
  CHECK(detected("subtitle/spruce.stl", "x.stl") == "spruce-stl");
  struct Case {
    std::string_view format;
    std::string_view fixture;
  };
  const Case cases[] = {
      {"ttml", "subtitle/timing_forms.ttml"}, {"dfxp", "subtitle/pycaption.dfxp"},
      {"ebu-tt", "subtitle/ebu_tt_d.xml"},    {"smpte-tt", "subtitle/smpte_tt.xml"},
      {"smi", "subtitle/pycaption.smi"},     {"srv3", "subtitle/youtube.srv3"},
      {"json3", "subtitle/youtube.json3"},    {"scc", "subtitle/pycaption.scc"},
      {"scc", "subtitle/rollup_painton.scc"}, {"ebu-stl", "subtitle/handmade.stl"},
  };
  for (const Case& c : cases) {
    INFO(c.format);
    CHECK(test::fuzz_reader<ir::Subtitles>(c.format, test::fixture(c.fixture)) > 0);
    CHECK_STATUS(test::read_as<ir::Subtitles>(c.format, test::random_bytes(1100, 3)), Status::Decode);
  }
  // Deeply nested spans and hostile frame numbers do not crash.
  std::string deep = "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div><p begin=\"0s\" end=\"1s\">";
  for (int i = 0; i < 5000; ++i) deep += "<span>";
  deep += "x</p></div></body></tt>";
  (void)test::read_as<ir::Subtitles>("ttml", test::bytes(deep));
  CHECK_OK(test::read_as<ir::Subtitles>("ttml", test::bytes("<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><p begin=\"99999999999999h\" end=\"1e300s\">x</p></body></tt>")));
}
