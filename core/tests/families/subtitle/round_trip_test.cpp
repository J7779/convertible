// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Round trips between every pair of writable text subtitle formats, the
// shared processing options, writer options and the bridges.
#include "families/subtitle/subtitle_test_support.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/plain_text.hpp"
#include "ir/table.hpp"

using namespace cvt;
using namespace cvt::test::subs;

namespace {

struct Writable {
  std::string_view id;
  std::int64_t tolerance_ms;  ///< time resolution of the format
  bool markup;                ///< keeps <i>/<b>/<u>
  bool exact_ends;            ///< stores end times
};

constexpr Writable writable[] = {
    {"srt", 0, true, true},         {"vtt", 0, true, true},       {"ass", 5, true, true},
    {"ssa", 5, true, true},         {"sbv", 0, false, true},      {"subviewer", 5, false, true},
    {"microdvd", 21, false, true},  {"mpl2", 50, false, true},    {"tmplayer", 1000, false, false},
    {"qttext", 0, false, true},     {"spruce-stl", 20, true, true}, {"lrc", 5, false, true},
};

}  // namespace

TEST_CASE("subtitle: every writable format round-trips the sample track") {
  const ir::Subtitles track = sample_track();
  for (const Writable& format : writable) {
    INFO(format.id);
    const auto encoded = test::write_from(format.id, track);
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Subtitles>(format.id, *encoded);
    REQUIRE_OK(decoded);
    check_cues(*decoded, expected_of(track), format.tolerance_ms, !format.markup, format.exact_ends);
  }
}

TEST_CASE("subtitle: every pair of writable formats converts through the engine") {
  const ir::Subtitles track = sample_track();
  for (const Writable& from : writable) {
    const auto source = test::write_from(from.id, track);
    REQUIRE_OK(source);
    for (const Writable& to : writable) {
      INFO(from.id, " -> ", to.id);
      const test::Converted converted = test::convert(*source, from.id, to.id);
      REQUIRE(converted.ok());
      const auto decoded = test::read_as<ir::Subtitles>(to.id, converted.data);
      REQUIRE_OK(decoded);
      const bool markup = from.markup && to.markup;
      check_cues(*decoded, expected_of(track), from.tolerance_ms + to.tolerance_ms, !markup,
                 from.exact_ends && to.exact_ends);
    }
  }
}

TEST_CASE("subtitle/srt: same-format conversion cleans up") {
  // Out of order, an empty cue, an unclosed tag and an overlap.
  const std::string_view messy =
      "2\n00:00:05,000 --> 00:00:06,000\nsecond\n\n"
      "1\n00:00:01,000 --> 00:00:05,500\n<i>first\n\n"
      "3\n00:00:07,000 --> 00:00:08,000\n   \n";
  const test::Converted cleaned = test::convert(messy, "srt", "srt", R"({"fix_overlaps":true})");
  REQUIRE(cleaned.ok());
  CHECK(cleaned.text() == "1\n00:00:01,000 --> 00:00:05,000\n<i>first</i>\n\n2\n00:00:05,000 --> 00:00:06,000\nsecond\n\n");
}

TEST_CASE("subtitle: time shift and frame-rate retiming") {
  const ir::Subtitles track = sample_track();
  const auto srt = test::write_from("srt", track);
  REQUIRE_OK(srt);
  const auto shifted = test::read_as<ir::Subtitles>("srt", *srt, R"({"offset_ms":-1500})");
  REQUIRE_OK(shifted);
  CHECK(shifted->cues[0].start_ms == 0);  // clamped at zero
  CHECK(shifted->cues[0].end_ms == 1000);
  CHECK(shifted->cues[1].start_ms == 1500);
  const auto later = test::read_as<ir::Subtitles>("srt", *srt, R"({"offset_ms":250})");
  REQUIRE_OK(later);
  CHECK(later->cues[4].start_ms == 3601250);
  // 25 -> 23.976 fps PAL speed-up undone: every time grows by 25/23.976.
  const auto retimed = test::read_as<ir::Subtitles>("srt", *srt, R"({"fps_from":25,"fps_to":23.976})");
  REQUIRE_OK(retimed);
  CHECK(retimed->cues[1].start_ms == 3128);
  CHECK(retimed->cues[4].start_ms == std::llround(3601000 * 25.0 / 23.976));
  // Only one of the two rates: nothing happens.
  const auto unchanged = test::read_as<ir::Subtitles>("srt", *srt, R"({"fps_from":25})");
  REQUIRE_OK(unchanged);
  CHECK(unchanged->cues[1].start_ms == 3000);
}

TEST_CASE("subtitle: duration, overlap, formatting and line options") {
  const std::string_view input =
      "1\n00:00:01,000 --> 00:00:01,100\n<i>A very short cue that is far too long for one line of text</i>\n\n"
      "2\n00:00:01,500 --> 00:00:30,000\nSecond\nwith two lines\n\n"
      "3\n00:00:01,500 --> 00:00:02,000\nSame start\n\n"
      "4\n00:00:40,000 --> 00:00:41,000\n\n";
  SUBCASE("minimum and maximum duration") {
    const auto subs = test::read_as<ir::Subtitles>("srt", test::bytes(input),
                                                   R"({"min_duration_ms":1000,"max_duration_ms":10000})");
    REQUIRE_OK(subs);
    CHECK(subs->cues[0].end_ms == 1500);   // extended, but not past the next start
    CHECK(subs->cues[1].end_ms == 11500);  // capped
  }
  SUBCASE("overlap repair merges cues that start together") {
    const auto subs = test::read_as<ir::Subtitles>("srt", test::bytes(input), R"({"fix_overlaps":true})");
    REQUIRE_OK(subs);
    REQUIRE(subs->cues.size() == 2);
    CHECK(subs->cues[1].text == "Second\nwith two lines\nSame start");
    CHECK(subs->cues[1].end_ms == 30000);
  }
  SUBCASE("formatting removal, line merging and wrapping") {
    const auto plain = test::read_as<ir::Subtitles>("srt", test::bytes(input), R"({"strip_formatting":true})");
    REQUIRE_OK(plain);
    CHECK(plain->cues[0].text == "A very short cue that is far too long for one line of text");
    const auto merged = test::read_as<ir::Subtitles>("srt", test::bytes(input), R"({"merge_lines":true})");
    REQUIRE_OK(merged);
    CHECK(merged->cues[1].text == "Second with two lines");
    const auto wrapped = test::read_as<ir::Subtitles>("srt", test::bytes(input), R"({"max_line_length":42})");
    REQUIRE_OK(wrapped);
    CHECK(wrapped->cues[0].text == "<i>A very short cue that is far</i>\n<i>too long for one line of text</i>" ||
          wrapped->cues[0].text == "<i>A very short cue that is far\ntoo long for one line of text</i>");
  }
  SUBCASE("empty cues are dropped unless asked to keep them") {
    const auto dropped = test::read_as<ir::Subtitles>("srt", test::bytes(input));
    REQUIRE_OK(dropped);
    CHECK(dropped->cues.size() == 3);
    const auto kept = test::read_as<ir::Subtitles>("srt", test::bytes(input), R"({"drop_empty":false})");
    REQUIRE_OK(kept);
    CHECK(kept->cues.size() == 4);
  }
}

TEST_CASE("subtitle: writer options") {
  const ir::Subtitles track = sample_track();
  const auto lf = test::write_from("srt", track);
  const auto crlf = test::write_from("srt", track, R"({"line_ending":"crlf","bom":true})");
  REQUIRE_OK(lf);
  REQUIRE_OK(crlf);
  CHECK(test::str(*crlf).starts_with("\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n"));
  const auto back = test::read_as<ir::Subtitles>("srt", *crlf);
  REQUIRE_OK(back);
  check_cues(*back, expected_of(track), 0, false);
  // MicroDVD frame rate and header line.
  const auto dvd = test::write_from("microdvd", track, R"({"fps":25,"microdvd_header":false})");
  REQUIRE_OK(dvd);
  CHECK(test::str(*dvd).starts_with("{25}{63}First line\n"));  // 2.5 s = 62.5 frames, rounded
  const auto dvd_header = test::write_from("microdvd", track, R"({"fps":30})");
  REQUIRE_OK(dvd_header);
  CHECK(test::str(*dvd_header).starts_with("{1}{1}30\n{30}{75}First line\n"));
  // Formatting that a target cannot hold is reported.
  std::vector<std::string> warnings;
  REQUIRE_OK(test::write_from("subviewer", track, {}, &warnings));
  CHECK(!warnings.empty());
}

TEST_CASE("subtitle/vtt: positions, voices and regions survive vtt -> vtt") {
  const test::Converted again = test::convert(test::fixture("subtitle/features.vtt"), "vtt", "vtt");
  REQUIRE(again.ok());
  const auto original = test::read_as<ir::Subtitles>("vtt", test::fixture("subtitle/features.vtt"));
  const auto copy = test::read_as<ir::Subtitles>("vtt", again.data);
  REQUIRE_OK(original);
  REQUIRE_OK(copy);
  REQUIRE(copy->cues.size() == original->cues.size());
  for (std::size_t i = 0; i < copy->cues.size(); ++i) {
    INFO("cue ", i);
    CHECK(copy->cues[i].text == original->cues[i].text);
    CHECK(copy->cues[i].speaker == original->cues[i].speaker);
    CHECK(copy->cues[i].style == original->cues[i].style);
    CHECK(copy->cues[i].attributes == original->cues[i].attributes);
  }
  CHECK(copy->metadata == original->metadata);
}

TEST_CASE("subtitle/ass: styles, override tags and positions survive ass -> ass and ass -> ssa") {
  const std::string_view script =
      "[Script Info]\nTitle: Styled\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n"
      "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
      "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
      "MarginL, MarginR, MarginV, Encoding\n"
      "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
      "Style: Thought,Georgia,22,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,-1,0,0,100,100,0,0,1,2,2,8,10,10,10,1\n\n"
      "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "Dialogue: 1,0:00:01.00,0:00:02.00,Default,Ada,0,0,0,,{\\c&H0000FF&}Red{\\c} and {\\i1}italic{\\i0}\\Nnext\n"
      "Dialogue: 0,0:00:03.00,0:00:04.00,Thought,,0,0,0,,Thinking\n"
      "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,{\\an9\\pos(320,180)}Placed\n"
      "Comment: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,Not shown\n";
  const auto subs = test::read_as<ir::Subtitles>("ass", test::bytes(script));
  REQUIRE_OK(subs);
  REQUIRE(subs->cues.size() == 3);
  CHECK(subs->cues[0].text == "Red and <i>italic</i>\nnext");
  CHECK(subs->cues[0].source_format == "ass");  // the colour tag cannot be mapped
  CHECK(subs->cues[0].attributes.at("ass.layer") == "1");
  CHECK(subs->cues[1].text == "<i>Thinking</i>");  // italic style
  REQUIRE(subs->cues[1].position.has_value());
  CHECK(subs->cues[1].position->alignment == 8);
  REQUIRE(subs->cues[2].position.has_value());
  CHECK(subs->cues[2].position->alignment == 9);
  CHECK_APPROX(subs->cues[2].position->x.value_or(0), 0.5, 1e-9);

  const test::Converted ass = test::convert(script, "ass", "ass");
  REQUIRE(ass.ok());
  CHECK(ass.text().find("{\\c&H0000FF&}Red{\\c} and {\\i1}italic{\\i0}\\Nnext") != std::string::npos);
  CHECK(ass.text().find("Style: Thought,Georgia,22") != std::string::npos);
  CHECK(ass.text().find("Dialogue: 0,0:00:03.00,0:00:04.00,Thought,,0,0,0,,Thinking") != std::string::npos);
  CHECK(ass.text().find("Not shown") == std::string::npos);

  const test::Converted ssa = test::convert(script, "ass", "ssa");
  REQUIRE(ssa.ok());
  CHECK(ssa.text().find("[V4 Styles]") != std::string::npos);
  const auto back = test::read_as<ir::Subtitles>("ssa", ssa.data);
  REQUIRE_OK(back);
  REQUIRE(back->cues.size() == 3);
  CHECK(back->cues[1].text == "<i>Thinking</i>");
  CHECK(back->cues[1].position.value_or(ir::CuePosition{}).alignment == 8);
  CHECK(back->cues[2].position.value_or(ir::CuePosition{}).alignment == 9);
}

TEST_CASE("subtitle: bridges to table, data, transcript and document") {
  ir::Subtitles subs = sample_track();
  subs.metadata["title"] = "Episode";
  subs.cues[0].speaker = "Ada";
  subs.cues[1].speaker = "Ada";
  subs.cues[2].speaker = "Ben";

  const auto table = run_bridge(Kind::Table, subs);
  REQUIRE_OK(table);
  const ir::Sheet& sheet = table->get<ir::Table>().sheets.at(0);
  REQUIRE(sheet.rows.size() == 6);
  CHECK(sheet.rows[0][0].text == "start");
  CHECK(sheet.rows[2][0].text == "00:00:03.000");
  CHECK(sheet.rows[2][2].number == 2000.0);
  CHECK(sheet.rows[2][3].text == "Ada");
  CHECK(sheet.rows[2][5].text == "Two lines\nwith ünïcödé");

  const auto data = run_bridge(Kind::Data, subs);
  REQUIRE_OK(data);
  const Json& json = data->get<ir::Data>().value;
  CHECK(json["metadata"]["title"].as_string() == "Episode");
  CHECK(json["cues"][1]["start_ms"].as_int64() == 3000);
  CHECK(json["cues"][1]["text"].as_string() == "<i>Two lines</i>\nwith ünïcödé");

  const auto text = run_bridge(Kind::PlainText, subs);
  REQUIRE_OK(text);
  CHECK(text->get<ir::PlainText>().text ==
        "Ada: First line Two lines with ünïcödé\n\nBen: Ελληνικά and שלום 🎬\n\nBold & under\n\nAfter an hour\n");
  const auto stamped = run_bridge(Kind::PlainText, subs, R"({"transcript_timestamps":true,"paragraph_gap_ms":0})");
  REQUIRE_OK(stamped);
  CHECK(stamped->get<ir::PlainText>().text.starts_with("[00:00:01] Ada: First line Two lines with ünïcödé\n\n[00:00:06] Ben:"));

  const auto document = run_bridge(Kind::Document, subs);
  REQUIRE_OK(document);
  const ir::Document& doc = document->get<ir::Document>();
  CHECK(doc.metadata.title == "Episode");
  CHECK(test::text_similarity(ir::plain_text(doc),
                              "Episode\n\nAda: First line Two lines with ünïcödé\n\nBen: Ελληνικά and שלום 🎬\n\n"
                              "Bold & under\n\nAfter an hour\n") == 1.0);

  // Through the engine: SRT to the plain-text writer of the encoding family.
  const auto srt = test::write_from("srt", subs);
  REQUIRE_OK(srt);
  const test::Converted transcript = test::convert(*srt, "srt", "txt");
  REQUIRE(transcript.ok());
  CHECK(transcript.text().find("Two lines with ünïcödé") != std::string::npos);
}
