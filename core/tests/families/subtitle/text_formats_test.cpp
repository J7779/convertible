// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Decoding of real files written by pysubs2, ffmpeg and by hand (formats
// without a common writer), for the line-based text subtitle formats.
#include "families/subtitle/subtitle_test_support.hpp"
#include "engine/detect.hpp"

using namespace cvt;
using namespace cvt::test::subs;

namespace {

std::vector<Expected> plain_master() { return master_script(); }

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

}  // namespace

TEST_CASE("subtitle/srt: decodes a file written by pysubs2") {
  const auto subs = test::read_as<ir::Subtitles>("srt", test::fixture("subtitle/basic_pysubs2.srt"));
  REQUIRE_OK(subs);
  // pysubs2 drops ASS bold when writing SRT; italics survive.
  check_cues(*subs, plain_master(), 0, true);
  CHECK(subs->cues[1].text == "<i>Café déjà vu</i>\nsecond line");
}

TEST_CASE("subtitle/srt: tolerates real-world mess") {
  std::vector<std::string> warnings;
  const auto subs = test::read_as<ir::Subtitles>("srt", test::fixture("subtitle/messy.srt"), {}, &warnings);
  REQUIRE_OK(subs);
  REQUIRE(subs->cues.size() == 4);
  // BOM, coordinates after the end time, {\an8} and <font> tags.
  CHECK(subs->cues[0].start_ms == 1000);
  CHECK(subs->cues[0].end_ms == 3500);
  CHECK(subs->cues[0].text == "Top line here");
  REQUIRE(subs->cues[0].position.has_value());
  CHECK(subs->cues[0].position->alignment == 8);
  // Missing index, '.' before the milliseconds, upper-case and unclosed tags.
  CHECK(subs->cues[1].start_ms == 4000);
  CHECK(subs->cues[1].end_ms == 6250);
  CHECK(subs->cues[1].text == "<i>Italic</i> and <b>bold</b>");
  // One-digit fields are decimal fractions (as pysubs2 reads them).
  CHECK(subs->cues[2].start_ms == 7500);
  CHECK(subs->cues[2].end_ms == 9250);
  CHECK(subs->cues[2].text == "Short fields & an entity");
  CHECK(subs->cues[3].start_ms == 10000);
  CHECK(subs->cues[3].text == "No spaces around the arrow");
}

TEST_CASE("subtitle/srt: detects legacy code pages and honours an explicit encoding") {
  struct Case {
    std::string_view file;
    std::string_view expected;
  };
  const Case cases[] = {
      {"subtitle/cp1251.srt", "Привет, мир! Это проверка кодировки."},
      {"subtitle/cp1250.srt", "Příliš žluťoučký kůň úpěl ďábelské ódy."},
      {"subtitle/cp1253.srt", "Γειά σου κόσμε, αυτή είναι μια δοκιμή."},
      {"subtitle/cp1252.srt", "Ça fait déjà très longtemps, garçon."},
      {"subtitle/utf16le.srt", "UTF-16 text — ünïcödé ✓"},
  };
  for (const Case& c : cases) {
    INFO(c.file);
    const auto subs = test::read_as<ir::Subtitles>("srt", test::fixture(c.file));
    REQUIRE_OK(subs);
    REQUIRE(!subs->cues.empty());
    CHECK(subs->cues[0].text == c.expected);
  }
  // Forcing a wrong code page gives different text; the right one matches.
  const auto forced = test::read_as<ir::Subtitles>("srt", test::fixture("subtitle/cp1251.srt"),
                                                   R"({"source_encoding":"windows-1251"})");
  REQUIRE_OK(forced);
  CHECK(forced->cues[1].text == "Съешь же ещё этих мягких французских булок.");
  const auto latin = test::read_as<ir::Subtitles>("srt", test::fixture("subtitle/cp1251.srt"),
                                                  R"({"source_encoding":"windows-1252"})");
  REQUIRE_OK(latin);
  CHECK(latin->cues[0].text != "Привет, мир! Это проверка кодировки.");
  CHECK_STATUS(test::read_as<ir::Subtitles>("srt", test::fixture("subtitle/cp1251.srt"), R"({"source_encoding":"klingon"})"),
               Status::InvalidArgument);
}

TEST_CASE("subtitle/vtt: decodes files written by pysubs2 and ffmpeg") {
  const auto py = test::read_as<ir::Subtitles>("vtt", test::fixture("subtitle/basic_pysubs2.vtt"));
  REQUIRE_OK(py);
  check_cues(*py, plain_master(), 0, true);
  const auto ff = test::read_as<ir::Subtitles>("vtt", test::fixture("subtitle/basic_ffmpeg.vtt"));
  REQUIRE_OK(ff);  // short mm:ss.ttt time stamps
  check_cues(*ff, plain_master(), 0, true);
  CHECK(ff->cues[1].text == "<i>Café déjà vu</i>\nsecond line");
}

TEST_CASE("subtitle/vtt: settings, regions, voices, styles and notes") {
  const auto subs = test::read_as<ir::Subtitles>("vtt", test::fixture("subtitle/features.vtt"));
  REQUIRE_OK(subs);
  REQUIRE(subs->cues.size() == 4);
  CHECK(subs->metadata.at("title") == "- Fixture track");
  CHECK(subs->metadata.at("language") == "en");
  CHECK(subs->metadata.contains("vtt.region.upper"));
  CHECK(subs->metadata.at("vtt.style").find("::cue(.loud)") != std::string::npos);
  const ir::Cue& first = subs->cues[0];
  CHECK(first.text == "Hello from the top left");
  REQUIRE(first.speaker.has_value());
  CHECK(*first.speaker == "Ada Example");
  REQUIRE(first.position.has_value());
  CHECK(first.position->alignment == 7);  // top left
  CHECK_APPROX(first.position->x.value_or(-1), 0.2, 1e-9);
  CHECK_APPROX(first.position->y.value_or(-1), 0.1, 1e-9);
  CHECK(first.attributes.at("vtt.id") == "intro");
  CHECK(subs->cues[1].style.value_or("") == "upper");
  CHECK(subs->cues[1].text == "Loud <i>italic</i> <b>bold</b> <u>under</u> & <tag>");
  CHECK(subs->cues[2].start_ms == 7000);
  CHECK(subs->cues[2].speaker.value_or("") == "Ben");
  CHECK(subs->cues[2].text == "Short timestamps with 漢 ruby");
  CHECK(subs->cues[3].text == "Karaoke timed words");
}

TEST_CASE("subtitle/ass: decodes files written by pysubs2 and ffmpeg") {
  const auto py = test::read_as<ir::Subtitles>("ass", test::fixture("subtitle/basic_pysubs2.ass"));
  REQUIRE_OK(py);
  check_cues(*py, plain_master(), 5, false);  // centisecond times: 12.345 s is stored as 12.35 s
  CHECK(py->metadata.at("title") == "Fixture script");
  CHECK(py->cues[0].speaker.value_or("") == "Ada");
  CHECK(py->cues[2].speaker.value_or("") == "Ben");
  const ir::Cue& top = py->cues[6];
  CHECK(top.style.value_or("") == "Top");
  REQUIRE(top.position.has_value());
  CHECK(top.position->alignment == 8);
  CHECK_APPROX(top.position->x.value_or(-1), 0.5, 1e-9);
  CHECK_APPROX(top.position->y.value_or(-1), 100.0 / 1080.0, 1e-9);

  const auto ssa = test::read_as<ir::Subtitles>("ssa", test::fixture("subtitle/basic_pysubs2.ssa"));
  REQUIRE_OK(ssa);
  check_cues(*ssa, plain_master(), 5, false);
  REQUIRE(ssa->cues[6].position.has_value());
  CHECK(ssa->cues[6].position->alignment == 8);  // legacy alignment 6 = top centre

  const auto ff = test::read_as<ir::Subtitles>("ass", test::fixture("subtitle/basic_ffmpeg.ass"));
  REQUIRE_OK(ff);
  check_cues(*ff, plain_master(), 5, true);
}

TEST_CASE("subtitle/microdvd: decodes a 25 fps file written by pysubs2") {
  const auto subs = test::read_as<ir::Subtitles>("microdvd", test::fixture("subtitle/microdvd_25fps_pysubs2.sub"));
  REQUIRE_OK(subs);
  check_cues(*subs, plain_master(), 40, true);  // one frame at 25 fps
  CHECK(subs->metadata.at("fps") == "25");
  // An explicit rate overrides the one in the file.
  const auto slow = test::read_as<ir::Subtitles>("microdvd", test::fixture("subtitle/microdvd_25fps_pysubs2.sub"),
                                                 R"({"fps":50})");
  REQUIRE_OK(slow);
  CHECK(slow->cues[0].start_ms == 500);
}

TEST_CASE("subtitle/mpl2 and tmplayer: decode files written by pysubs2") {
  const auto mpl2 = test::read_as<ir::Subtitles>("mpl2", test::fixture("subtitle/mpl2_pysubs2.txt"));
  REQUIRE_OK(mpl2);
  check_cues(*mpl2, plain_master(), 100, true);  // deciseconds (pysubs2 truncates)
  const auto tmp = test::read_as<ir::Subtitles>("tmplayer", test::fixture("subtitle/tmplayer_pysubs2.txt"));
  REQUIRE_OK(tmp);
  // TMPlayer stores whole-second start times only.
  check_cues(*tmp, plain_master(), 999, true, false);
  CHECK(tmp->cues[1].text == "<i>Café déjà vu</i>\nsecond line");
  CHECK(tmp->cues[0].end_ms > tmp->cues[0].start_ms);
  CHECK(tmp->cues[0].end_ms <= tmp->cues[1].start_ms);
}

TEST_CASE("subtitle/sbv, subviewer, qttext, spruce-stl: decode hand-written files") {
  const std::vector<Expected> two = {{1000, 3500, "Hello, Ada Example."}, {4000, 6250, "Café déjà vu\nsecond line"}};
  const auto sbv = test::read_as<ir::Subtitles>("sbv", test::fixture("subtitle/youtube.sbv"));
  REQUIRE_OK(sbv);
  REQUIRE(sbv->cues.size() == 3);
  CHECK(sbv->cues[2].start_ms == 3601000);
  const auto sv2 = test::read_as<ir::Subtitles>("subviewer", test::fixture("subtitle/subviewer2.sub"));
  REQUIRE_OK(sv2);
  check_cues(*sv2, two, 0, false);
  CHECK(sv2->metadata.at("title") == "Fixture");
  const auto sv1 = test::read_as<ir::Subtitles>("subviewer", test::fixture("subtitle/subviewer1.sub"));
  REQUIRE_OK(sv1);
  check_cues(*sv1, {{1000, 3000, "Hello, Ada Example."}, {4000, 6000, "Café déjà vu\nsecond line"}}, 0, false);
  // {timeScale:30}: ".15" is 15/30 s.
  const auto qt = test::read_as<ir::Subtitles>("qttext", test::fixture("subtitle/quicktime.txt"));
  REQUIRE_OK(qt);
  check_cues(*qt, {{1000, 3500, "Hello, Ada Example."}, {4000, 6200, "Café déjà vu\nsecond line"}}, 0, false);
  // Spruce time codes end in frames (25 fps by default).
  const auto stl = test::read_as<ir::Subtitles>("spruce-stl", test::fixture("subtitle/spruce.stl"));
  REQUIRE_OK(stl);
  check_cues(*stl, {{1000, 3480, "Hello, Ada Example."}, {4000, 6240, "<i>Café déjà vu</i>\nsecond line"}}, 0, false);
  const auto stl30 = test::read_as<ir::Subtitles>("spruce-stl", test::fixture("subtitle/spruce.stl"), R"({"fps":30})");
  REQUIRE_OK(stl30);
  CHECK(stl30->cues[0].end_ms == 3400);
}

TEST_CASE("subtitle/lrc: decodes simple and enhanced lyrics") {
  const auto ff = test::read_as<ir::Subtitles>("lrc", test::fixture("subtitle/basic_ffmpeg.lrc"));
  REQUIRE_OK(ff);
  REQUIRE(ff->cues.size() == 8);
  // ffmpeg writes start times only: each line lasts until the next one.
  CHECK(ff->cues[0].start_ms == 1000);
  CHECK(ff->cues[0].end_ms == 4000);
  CHECK(ff->cues[1].text == "<i>Café déjà vu</i>\nsecond line");  // two lines with one stamp
  CHECK(ff->cues[7].start_ms == 3601000);

  const auto enhanced = test::read_as<ir::Subtitles>("lrc", test::fixture("subtitle/enhanced.lrc"));
  REQUIRE_OK(enhanced);
  CHECK(enhanced->metadata.at("title") == "Fixture Song");
  CHECK(enhanced->metadata.at("artist") == "Ada Example");
  check_cues(*enhanced,
             {{1000, 3500, "First line"},
              {3500, 7750, "Repeated chorus line"},
              {7750, 11500, "Word by word"},  // word time stamps removed; the empty line ends the cue
              {14500, 19500, "Last line"},
              {19500, 60000, "Repeated chorus line"}},  // [length:01:00] ends the last line
             0, false);
}

TEST_CASE("subtitle: formats are detected by content and name") {
  CHECK(detected("subtitle/basic_pysubs2.srt", "a.srt") == "srt");
  CHECK(detected("subtitle/messy.srt", "") == "srt");
  CHECK(detected("subtitle/basic_ffmpeg.vtt", "") == "vtt");
  CHECK(detected("subtitle/basic_pysubs2.ass", "") == "ass");
  CHECK(detected("subtitle/basic_pysubs2.ssa", "") == "ssa");
  CHECK(detected("subtitle/basic_pysubs2.ssa", "x.ssa") == "ssa");
  CHECK(detected("subtitle/microdvd_25fps_pysubs2.sub", "x.sub") == "microdvd");
  CHECK(detected("subtitle/subviewer2.sub", "x.sub") == "subviewer");
  CHECK(detected("subtitle/subviewer1.sub", "") == "subviewer");
  CHECK(detected("subtitle/youtube.sbv", "") == "sbv");
  CHECK(detected("subtitle/mpl2_pysubs2.txt", "x.txt") == "mpl2");
  CHECK(detected("subtitle/tmplayer_pysubs2.txt", "x.txt") == "tmplayer");
  CHECK(detected("subtitle/quicktime.txt", "x.txt") == "qttext");
  CHECK(detected("subtitle/spruce.stl", "x.stl") == "spruce-stl");
  CHECK(detected("subtitle/enhanced.lrc", "") == "lrc");
  CHECK(detected("subtitle/basic_ffmpeg.lrc", "x.lrc") == "lrc");
}

TEST_CASE("subtitle: damaged and hostile input never crashes") {
  struct Case {
    std::string_view format;
    std::string_view fixture;
  };
  const Case cases[] = {
      {"srt", "subtitle/messy.srt"},          {"vtt", "subtitle/features.vtt"},
      {"ass", "subtitle/basic_pysubs2.ass"},  {"ssa", "subtitle/basic_pysubs2.ssa"},
      {"microdvd", "subtitle/microdvd_25fps_pysubs2.sub"}, {"mpl2", "subtitle/mpl2_pysubs2.txt"},
      {"tmplayer", "subtitle/tmplayer_pysubs2.txt"},       {"sbv", "subtitle/youtube.sbv"},
      {"subviewer", "subtitle/subviewer2.sub"},            {"subviewer", "subtitle/subviewer1.sub"},
      {"qttext", "subtitle/quicktime.txt"},                {"spruce-stl", "subtitle/spruce.stl"},
      {"lrc", "subtitle/enhanced.lrc"},
  };
  for (const Case& c : cases) {
    INFO(c.format);
    CHECK(test::fuzz_reader<ir::Subtitles>(c.format, test::fixture(c.fixture)) > 0);
    CHECK_STATUS(test::read_as<ir::Subtitles>(c.format, test::random_bytes(300, 7)), Status::Decode);
  }
  // Absurd numbers are clamped instead of overflowing.
  const auto huge = test::read_as<ir::Subtitles>("srt", test::bytes("1\n999999999:59:59,999 --> 999999999:59:59,999\nx\n"));
  CHECK(!huge.has_value() || huge->cues.empty() || huge->cues[0].start_ms <= 3'600'000'000LL);
  const auto frames = test::read_as<ir::Subtitles>("microdvd", test::bytes("{999999999}{999999999}x\n"));
  REQUIRE_OK(frames);
  // An empty file is an empty track.
  const auto empty = test::read_as<ir::Subtitles>("srt", test::bytes(""));
  REQUIRE_OK(empty);
  CHECK(empty->cues.empty());
  CHECK_STATUS(test::read_as<ir::Subtitles>("vtt", test::bytes("not vtt\n")), Status::Decode);
}
