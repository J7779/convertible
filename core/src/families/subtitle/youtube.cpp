// Convertible core - subtitle family: YouTube json3 transcripts.
//
// {"wireMagic":"pb3","events":[{"tStartMs":1000,"dDurationMs":2500,
//   "segs":[{"utf8":"Hello "},{"utf8":"world","tOffsetMs":400}]}, ...]}
// Window-setup events (no "segs") and append events ("aAppend") carry no
// new captions and are skipped; a segment of just "\n" is a line break.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "families/subtitle/subtitle.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

Result<ir::Subtitles> parse_json3(std::string_view text, const ReadContext& /*ctx*/) {
  CVT_TRY_ASSIGN(const Json root, util::parse_json(text, 64));
  const Json* events = root.find("events");
  if (events == nullptr || !events->is_array()) return fail(Status::Decode, "not a YouTube json3 file (no \"events\" array)");
  ir::Subtitles subs;
  for (const Json& event : events->elements()) {
    const Json* segs = event.find("segs");
    if (segs == nullptr || !segs->is_array() || event["aAppend"].as_int64(0) != 0) continue;
    const std::int64_t start = event["tStartMs"].as_int64(-1);
    const std::int64_t duration = event["dDurationMs"].as_int64(0);
    if (start < 0 || start > 3'600'000'000LL || duration < 0 || duration > 3'600'000'000LL) continue;
    std::string body;
    for (const Json& seg : segs->elements()) body += seg["utf8"].as_string();
    std::string cleaned;
    for (const std::string_view line : util::split(body, '\n')) {
      if (util::trim(line).empty()) continue;
      if (!cleaned.empty()) cleaned.push_back('\n');
      cleaned.append(util::trim(line));
    }
    if (cleaned.empty()) continue;
    ir::Cue cue;
    cue.start_ms = start;
    cue.end_ms = start + duration;
    cue.text = cleaned;
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

Result<std::string> write_json3(const ir::Subtitles& subs, const WriteContext& ctx) {
  Json events = Json::array();
  bool dropped = false;
  for (const ir::Cue& cue : subs.cues) {
    const std::string plain = strip_markup(cue.text);
    dropped = dropped || plain != cue.text;
    Json segs = Json::array();
    segs.push_back(Json::object({{"utf8", plain}}));
    events.push_back(Json::object({{"tStartMs", cue.start_ms},
                                   {"dDurationMs", std::max<std::int64_t>(cue.end_ms - cue.start_ms, 0)},
                                   {"wWinId", 1},
                                   {"segs", std::move(segs)}}));
  }
  if (dropped) ctx.warn("Formatting was removed; json3 output carries plain text.");
  const Json root = Json::object({{"wireMagic", "pb3"}, {"events", std::move(events)}});
  return util::dump_json(root, 2) + "\n";
}

int sniff_json3(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 2048);
  if (!head.starts_with('{')) return 0;
  if (head.find("\"wireMagic\"") != std::string::npos) return 95;
  return head.find("\"events\"") != std::string::npos && head.find("\"tStartMs\"") != std::string::npos ? 90 : 0;
}

}  // namespace

void register_youtube(Registry& registry) {
  FormatInfo info = subtitle_format("json3", "YouTube json3 captions", "YouTube's JSON caption and transcript format.",
                                    {"json3", "json"}, "application/json", 10);
  info.sniff = sniff_json3;
  registry.add_format(std::move(info));
  registry.add_codec(make_text_codec("json3", parse_json3, write_json3));
}

}  // namespace cvt::subtitle
