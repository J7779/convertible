// Convertible core - subtitle family: line-based text subtitle formats.
//
//   srt         SubRip                    hh:mm:ss,mmm --> hh:mm:ss,mmm blocks
//   vtt         WebVTT                    W3C WebVTT (cue settings, regions, voices, STYLE/NOTE)
//   sbv         YouTube SBV               h:mm:ss.mmm,h:mm:ss.mmm blocks
//   subviewer   SubViewer 1 and 2         [INFORMATION] header, hh:mm:ss.cc,hh:mm:ss.cc
//   microdvd    MicroDVD                  {frame}{frame}text|line, frame based
//   mpl2        MPL2                      [ds][ds]text|/italic line, deciseconds
//   tmplayer    TMPlayer                  hh:mm:ss:text|line, start times only
//   qttext      QuickTime Text            {QTtext} header, [hh:mm:ss.ff] time stamps
//   spruce-stl  Spruce / DVD Studio STL   hh:mm:ss:ff , hh:mm:ss:ff , text
//   lrc         LRC lyrics                [mm:ss.xx]line, enhanced <mm:ss.xx> word times
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/subtitle/subtitle.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

using namespace std::string_view_literals;

bool all_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return util::is_ascii_digit(c); });
}

std::string join_lines(const std::vector<std::string>& lines) { return util::join(lines, "\n"); }

/// Text lines of a cue with blank lines removed.
std::string body_text(std::span<const std::string_view> lines) {
  std::vector<std::string> kept;
  for (const std::string_view line : lines) {
    if (!util::trim(line).empty()) kept.emplace_back(line);
  }
  return join_lines(kept);
}

/// Cues without end times end at the next cue, or after a reading time
/// derived from their length (1.5 s .. 5 s) when they are last.
void fill_open_ends(std::vector<ir::Cue>& cues, const std::vector<bool>& open_end) {
  std::vector<std::size_t> order(cues.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return cues[a].start_ms < cues[b].start_ms; });
  for (std::size_t k = 0; k < order.size(); ++k) {
    ir::Cue& cue = cues[order[k]];
    if (!open_end[order[k]]) continue;
    const std::int64_t reading = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(util::utf8_length(strip_markup(cue.text))) * 60 + 1000, 1500, 5000);
    std::int64_t end = cue.start_ms + reading;
    for (std::size_t n = k + 1; n < order.size(); ++n) {
      if (cues[order[n]].start_ms > cue.start_ms) {
        end = std::min(end, cues[order[n]].start_ms);
        break;
      }
    }
    cue.end_ms = end;
  }
}

/// Lines with '|' separators, each optionally starting with '/' (italic).
std::string pipe_text_to_ir(std::string_view text, bool slash_italics) {
  std::vector<std::string> lines;
  for (std::string_view line : util::split(text, '|')) {
    line = util::trim(line);
    if (slash_italics && line.starts_with('/')) {
      lines.push_back("<i>" + std::string(util::trim_left(line.substr(1))) + "</i>");
    } else {
      lines.emplace_back(line);
    }
  }
  return html_to_ir(join_lines(lines));
}

/// IR text to '|' separated lines; lines entirely in italics get `italic_prefix`.
std::string ir_to_pipe_text(std::string_view ir_text, std::string_view italic_prefix, bool* dropped = nullptr) {
  struct Line {
    std::string text;
    bool all_italic = true;
    bool any_italic = false;
    bool any_text = false;
  };
  std::vector<Line> lines(1);
  bool italic = false;
  bool other = false;
  for (const MarkupToken& token : tokenize_markup(balance_markup(ir_text))) {
    switch (token.type) {
      case MarkupToken::Type::Text:
        lines.back().text += token.text;
        lines.back().any_text = true;
        if (italic) lines.back().any_italic = true;
        else lines.back().all_italic = false;
        break;
      case MarkupToken::Type::Open:
      case MarkupToken::Type::Close:
        if (token.text == "i") italic = token.type == MarkupToken::Type::Open;
        else other = true;
        break;
      case MarkupToken::Type::LineBreak: lines.emplace_back(); break;
    }
  }
  std::vector<std::string> out;
  for (Line& line : lines) {
    const bool whole = line.any_text && line.all_italic && !italic_prefix.empty();
    if (line.any_italic && !whole) other = true;
    out.push_back(whole ? std::string(italic_prefix) + line.text : std::move(line.text));
  }
  if (dropped != nullptr && other) *dropped = true;
  return util::join(out, "|");
}

// ============================================================================ SubRip

/// Map the ASS-style tags found in many SRT files ({\an8}, {\i1}) to IR
/// markup and a position; other override blocks are dropped.
std::string srt_overrides(std::string_view text, ir::Cue& cue) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '\\') {
      const std::size_t close = text.find('}', i);
      if (close == std::string_view::npos) break;
      const std::string_view block = text.substr(i + 1, close - i - 1);
      for (const std::string_view tag : util::split(block, '\\', false)) {
        if (tag.starts_with("an") && tag.size() == 3 && tag[2] >= '1' && tag[2] <= '9') {
          ir::CuePosition position;
          position.alignment = tag[2] - '0';
          cue.position = position;
        } else if (tag == "i1" || tag == "i") out += "<i>";
        else if (tag == "i0") out += "</i>";
        else if (tag == "b1" || tag == "b") out += "<b>";
        else if (tag == "b0") out += "</b>";
        else if (tag == "u1" || tag == "u") out += "<u>";
        else if (tag == "u0") out += "</u>";
      }
      i = close + 1;
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

/// "00:00:01,000 --> 00:00:02,000 X1:..." -> (start, end).
std::optional<std::pair<std::int64_t, std::int64_t>> srt_timing(std::string_view line) {
  std::size_t arrow = line.find("-->");
  std::size_t arrow_length = 3;
  if (arrow == std::string_view::npos) {
    arrow = line.find("->");
    arrow_length = 2;
  }
  if (arrow == std::string_view::npos) return std::nullopt;
  const std::string_view left = util::trim(line.substr(0, arrow));
  std::string_view right = util::trim(line.substr(arrow + arrow_length));
  if (left.empty() || right.empty() || !util::is_ascii_digit(left.back())) return std::nullopt;
  auto start = parse_clock(left);
  auto end = parse_clock(right);
  if (!end) {
    // Coordinates or cue settings after the end time.
    std::size_t cut = 0;
    while (cut < right.size() && !util::is_ascii_space(right[cut])) ++cut;
    end = parse_clock(right.substr(0, cut));
  }
  if (!start || !end) return std::nullopt;
  return std::pair{*start, *end};
}

Result<ir::Subtitles> parse_srt(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  const std::vector<std::string_view> lines = text_lines(text);
  std::vector<std::size_t> timings;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find("->") != std::string_view::npos && srt_timing(lines[i])) timings.push_back(i);
  }
  if (timings.empty()) {
    if (util::trim(text).empty()) return subs;
    return fail(Status::Decode, "no SubRip time codes ('00:00:01,000 --> 00:00:02,000') found");
  }
  for (std::size_t k = 0; k < timings.size(); ++k) {
    const std::size_t first = timings[k] + 1;
    std::size_t last = k + 1 < timings.size() ? timings[k + 1] : lines.size();
    // The next cue's index line (and blank lines before it) belong to no cue.
    if (k + 1 < timings.size() && last > first && all_digits(util::trim(lines[last - 1]))) --last;
    while (last > first && util::trim(lines[last - 1]).empty()) --last;
    const auto [start, end] = *srt_timing(lines[timings[k]]);
    ir::Cue cue;
    cue.start_ms = start;
    cue.end_ms = end;
    cue.text = html_to_ir(srt_overrides(body_text(std::span(lines).subspan(first, last - first)), cue));
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

Result<std::string> write_srt(const ir::Subtitles& subs, const WriteContext& /*ctx*/) {
  std::string out;
  std::size_t index = 0;
  for (const ir::Cue& cue : subs.cues) {
    out += std::format("{}\n{} --> {}\n", ++index, format_clock(cue.start_ms), format_clock(cue.end_ms));
    if (cue.position && cue.position->alignment != 2 && cue.position->alignment >= 1 && cue.position->alignment <= 9) {
      out += std::format("{{\\an{}}}", cue.position->alignment);
    }
    out += cue.text;
    out += "\n\n";
  }
  return out;
}

int sniff_srt(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 1024);
  const std::vector<std::string_view> lines = text_lines(head);
  for (std::size_t i = 0; i < std::min<std::size_t>(lines.size(), 4); ++i) {
    if (lines[i].find("-->") == std::string_view::npos) continue;
    const auto timing = srt_timing(lines[i]);
    if (!timing || lines[i].find("WEBVTT") != std::string_view::npos) return 0;
    const bool comma = lines[i].find(',') != std::string_view::npos;
    const bool indexed = i > 0 && all_digits(util::trim(lines[i - 1]));
    return comma ? (indexed ? 92 : 85) : (indexed ? 80 : 60);
  }
  return 0;
}

// ============================================================================ WebVTT

struct VttSettings {
  std::optional<ir::CuePosition> position;
  std::string region;
};

VttSettings parse_vtt_settings(std::string_view settings) {
  VttSettings out;
  int row = 0;  // 0 bottom, 3 middle, 6 top
  int column = 2;
  bool any = false;
  ir::CuePosition position;
  for (const std::string_view token : util::split(settings, ' ', false)) {
    const std::size_t colon = token.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view key = token.substr(0, colon);
    std::string_view value = token.substr(colon + 1);
    const std::string_view first_value = value.substr(0, value.find(','));
    if (key == "region") {
      out.region = std::string(value);
    } else if (key == "align") {
      any = true;
      if (value == "start" || value == "left") column = 1;
      else if (value == "end" || value == "right") column = 3;
      else column = 2;
    } else if (key == "line") {
      any = true;
      if (first_value.ends_with('%')) {
        const auto percent = util::parse_double(first_value.substr(0, first_value.size() - 1));
        if (percent && *percent >= 0 && *percent <= 100) {
          position.y = *percent / 100.0;
          row = *percent < 34 ? 6 : *percent < 67 ? 3 : 0;
        }
      } else if (const auto number = util::parse_int(first_value)) {
        row = *number < 0 ? 0 : (*number <= 3 ? 6 : 3);
      }
    } else if (key == "position") {
      any = true;
      if (first_value.ends_with('%')) {
        const auto percent = util::parse_double(first_value.substr(0, first_value.size() - 1));
        if (percent && *percent >= 0 && *percent <= 100) position.x = *percent / 100.0;
      }
    } else if (key == "size" || key == "vertical") {
      any = true;
    }
  }
  if (any) {
    position.alignment = row + column;
    out.position = position;
  }
  return out;
}

std::string vtt_settings_for(const ir::CuePosition& position) {
  std::vector<std::string> parts;
  const int column = (position.alignment - 1) % 3;  // 0 left, 1 centre, 2 right
  const int row = (position.alignment - 1) / 3;     // 0 bottom, 1 middle, 2 top
  if (position.y) {
    parts.push_back(std::format("line:{}%", util::format_double(std::round(*position.y * 10000.0) / 100.0)));
  } else if (row == 2) {
    parts.emplace_back("line:0");
  } else if (row == 1) {
    parts.emplace_back("line:50%");
  }
  if (position.x) parts.push_back(std::format("position:{}%", util::format_double(std::round(*position.x * 10000.0) / 100.0)));
  if (column == 0) parts.emplace_back("align:start");
  if (column == 2) parts.emplace_back("align:end");
  return util::join(parts, " ");
}

bool same_position(const std::optional<ir::CuePosition>& a, const std::optional<ir::CuePosition>& b) {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return true;
  const auto close = [](const std::optional<double>& x, const std::optional<double>& y) {
    return x.has_value() == y.has_value() && (!x || std::abs(*x - *y) < 1e-6);
  };
  return a->alignment == b->alignment && close(a->x, b->x) && close(a->y, b->y);
}

/// Voice spans give the speaker; ruby annotations are dropped; everything
/// else goes through the HTML-like markup mapping.
std::string vtt_payload_to_ir(std::string_view payload, ir::Cue& cue) {
  std::string text(payload);
  // <rt>...</rt> annotations are not part of the spoken text.
  for (std::size_t at = text.find("<rt>"); at != std::string::npos; at = text.find("<rt>", at)) {
    const std::size_t close = text.find("</rt>", at);
    text.erase(at, close == std::string::npos ? std::string::npos : close + 5 - at);
  }
  const std::size_t voice = text.find("<v");
  if (voice != std::string::npos && voice + 2 < text.size() && (text[voice + 2] == ' ' || text[voice + 2] == '.')) {
    const std::size_t close = text.find('>', voice);
    if (close != std::string::npos) {
      std::string_view annotation = std::string_view(text).substr(voice + 2, close - voice - 2);
      if (annotation.starts_with('.')) {
        const std::size_t space = annotation.find(' ');
        annotation = space == std::string_view::npos ? std::string_view() : annotation.substr(space);
      }
      const std::string name = html_to_ir(util::trim(annotation));
      if (!name.empty()) cue.speaker = name;
    }
  }
  return html_to_ir(text);
}

Result<ir::Subtitles> parse_vtt(std::string_view text, const ReadContext& /*ctx*/) {
  const std::vector<std::string_view> lines = text_lines(text);
  if (lines.empty() || !lines[0].starts_with("WEBVTT") ||
      (lines[0].size() > 6 && lines[0][6] != ' ' && lines[0][6] != '\t')) {
    return fail(Status::Decode, "a WebVTT file must start with 'WEBVTT'");
  }
  ir::Subtitles subs;
  // Blocks are separated by blank lines.
  std::vector<std::vector<std::string_view>> blocks;
  std::vector<std::string_view> current;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (util::trim(lines[i]).empty()) {
      if (!current.empty()) blocks.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(lines[i]);
    }
  }
  if (!current.empty()) blocks.push_back(std::move(current));
  // Header block: "WEBVTT" plus optional header lines (Kind:, Language:).
  std::vector<std::string> header_lines;
  for (std::size_t i = 1; i < blocks[0].size(); ++i) {
    const std::string_view line = blocks[0][i];
    if (line.find("-->") != std::string_view::npos) break;
    header_lines.emplace_back(line);
    if (util::istarts_with(line, "Language:")) subs.metadata["language"] = std::string(util::trim(line.substr(9)));
  }
  if (lines[0].size() > 7) subs.metadata["title"] = std::string(util::trim(lines[0].substr(7)));
  if (!header_lines.empty()) subs.metadata["vtt.header"] = util::join(header_lines, "\n");
  std::string styles;
  for (std::size_t b = 0; b < blocks.size(); ++b) {
    std::span<const std::string_view> block = blocks[b];
    if (b == 0) {
      // A cue may follow the header without a blank line in sloppy files.
      std::size_t skip = 1;
      while (skip < block.size() && block[skip].find("-->") == std::string_view::npos) ++skip;
      if (skip >= block.size()) continue;
      block = block.subspan(skip > 0 && skip - 1 > 0 && block[skip - 1].find(':') == std::string_view::npos ? skip - 1 : skip);
    }
    const std::string_view head = block[0];
    if (head.starts_with("NOTE") && (head.size() == 4 || head[4] == ' ' || head[4] == '\t')) continue;
    if (head == "STYLE" || head.starts_with("STYLE ")) {
      std::vector<std::string_view> css(block.begin() + 1, block.end());
      if (!styles.empty()) styles.push_back('\n');
      styles += util::join(css, "\n");
      continue;
    }
    if (head == "REGION" || head.starts_with("REGION ")) {
      std::string id;
      std::vector<std::string> settings;
      for (std::size_t i = 1; i < block.size(); ++i) {
        for (const std::string_view token : util::split(block[i], ' ', false)) {
          if (token.starts_with("id:")) id = std::string(token.substr(3));
          else settings.emplace_back(token);
        }
      }
      if (!id.empty()) subs.metadata["vtt.region." + id] = util::join(settings, " ");
      continue;
    }
    std::size_t timing_line = 0;
    std::string id;
    if (head.find("-->") == std::string_view::npos) {
      if (block.size() < 2 || block[1].find("-->") == std::string_view::npos) continue;  // not a cue
      id = std::string(head);
      timing_line = 1;
    }
    const std::string_view timing = block[timing_line];
    const std::size_t arrow = timing.find("-->");
    const auto start = parse_clock(util::trim(timing.substr(0, arrow)));
    std::string_view rest = util::trim(timing.substr(arrow + 3));
    std::size_t cut = 0;
    while (cut < rest.size() && !util::is_ascii_space(rest[cut])) ++cut;
    const auto end = parse_clock(rest.substr(0, cut));
    if (!start || !end) continue;
    const std::string_view settings = util::trim(rest.substr(cut));
    ir::Cue cue;
    cue.start_ms = *start;
    cue.end_ms = *end;
    const VttSettings parsed = parse_vtt_settings(settings);
    cue.position = parsed.position;
    if (!parsed.region.empty()) cue.style = parsed.region;
    if (!settings.empty()) cue.attributes["vtt.settings"] = std::string(settings);
    if (!id.empty()) cue.attributes["vtt.id"] = id;
    std::vector<std::string_view> payload(block.begin() + static_cast<std::ptrdiff_t>(timing_line) + 1, block.end());
    cue.text = vtt_payload_to_ir(util::join(payload, "\n"), cue);
    subs.cues.push_back(std::move(cue));
  }
  if (!styles.empty()) subs.metadata["vtt.style"] = styles;
  return subs;
}

Result<std::string> write_vtt(const ir::Subtitles& subs, const WriteContext& /*ctx*/) {
  std::string out = "WEBVTT";
  if (const auto title = subs.metadata.find("title"); title != subs.metadata.end() && !title->second.empty() &&
                                                       title->second.find('\n') == std::string::npos &&
                                                       title->second.find("-->") == std::string::npos) {
    out += " " + title->second;
  }
  out += "\n";
  if (const auto header = subs.metadata.find("vtt.header"); header != subs.metadata.end()) {
    for (const std::string_view line : util::split(header->second, '\n', false)) {
      if (line.find("-->") == std::string_view::npos) out += std::string(line) + "\n";
    }
  } else if (const auto language = subs.metadata.find("language"); language != subs.metadata.end()) {
    out += "Language: " + language->second + "\n";
  }
  out += "\n";
  for (const auto& [key, value] : subs.metadata) {
    if (key.starts_with("vtt.region.")) out += std::format("REGION\nid:{} {}\n\n", key.substr(11), value);
  }
  if (const auto style = subs.metadata.find("vtt.style"); style != subs.metadata.end()) {
    std::string css = util::replace_all(style->second, "-->", "--");
    std::vector<std::string> kept;
    for (const std::string_view line : util::split(css, '\n')) {
      if (!util::trim(line).empty()) kept.emplace_back(line);
    }
    out += "STYLE\n" + util::join(kept, "\n") + "\n\n";
  }
  for (const ir::Cue& cue : subs.cues) {
    if (const auto id = cue.attributes.find("vtt.id");
        id != cue.attributes.end() && id->second.find("-->") == std::string::npos && id->second.find('\n') == std::string::npos) {
      out += id->second + "\n";
    }
    const ClockStyle style{2, '.', 3};
    out += format_clock(cue.start_ms, style) + " --> " + format_clock(cue.end_ms, style);
    std::string settings;
    if (const auto raw = cue.attributes.find("vtt.settings");
        raw != cue.attributes.end() && same_position(parse_vtt_settings(raw->second).position, cue.position)) {
      settings = raw->second;
    } else {
      if (cue.position) settings = vtt_settings_for(*cue.position);
      if (cue.style && subs.metadata.contains("vtt.region." + *cue.style)) {
        settings += (settings.empty() ? "" : " ") + std::string("region:") + *cue.style;
      }
    }
    if (!settings.empty()) out += " " + settings;
    out += "\n";
    std::string body = ir_to_html(cue.text);
    // A blank line would end the cue early.
    std::vector<std::string> lines;
    for (const std::string_view line : util::split(body, '\n')) {
      if (!util::trim(line).empty()) lines.emplace_back(line);
    }
    body = util::join(lines, "\n");
    body = util::replace_all(body, "-->", "--&gt;");
    if (cue.speaker && !cue.speaker->empty()) body = "<v " + ir_to_html(*cue.speaker) + ">" + body;
    out += body + "\n\n";
  }
  return out;
}

int sniff_vtt(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 64);
  if (head.starts_with("WEBVTT") && (head.size() == 6 || head[6] == ' ' || head[6] == '\t' || head[6] == '\n' || head[6] == '\r')) {
    return 95;
  }
  return 0;
}

// ============================================================================ YouTube SBV

std::optional<std::pair<std::int64_t, std::int64_t>> sbv_timing(std::string_view line) {
  const std::vector<std::string_view> parts = util::split(line, ',');
  if (parts.size() != 2 || parts[0].find(':') == std::string_view::npos || parts[1].find(':') == std::string_view::npos) {
    return std::nullopt;
  }
  const auto start = parse_clock(parts[0]);
  const auto end = parse_clock(parts[1]);
  if (!start || !end) return std::nullopt;
  return std::pair{*start, *end};
}

Result<ir::Subtitles> parse_sbv(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  const std::vector<std::string_view> lines = text_lines(text);
  std::size_t i = 0;
  while (i < lines.size()) {
    const auto timing = sbv_timing(util::trim(lines[i]));
    if (!timing) {
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    while (j < lines.size() && !util::trim(lines[j]).empty()) ++j;
    ir::Cue cue;
    cue.start_ms = timing->first;
    cue.end_ms = timing->second;
    cue.text = html_to_ir(body_text(std::span(lines).subspan(i + 1, j - i - 1)));
    subs.cues.push_back(std::move(cue));
    i = j;
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no SBV time codes found");
  return subs;
}

Result<std::string> write_sbv(const ir::Subtitles& subs, const WriteContext& /*ctx*/) {
  std::string out;
  const ClockStyle style{1, '.', 3};
  for (const ir::Cue& cue : subs.cues) {
    out += format_clock(cue.start_ms, style) + "," + format_clock(cue.end_ms, style) + "\n";
    out += strip_markup(cue.text) + "\n\n";
  }
  return out;
}

int sniff_sbv(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 512);
  const std::vector<std::string_view> lines = text_lines(head);
  if (lines.empty()) return 0;
  const std::string_view first = util::trim(lines[0]);
  // h:mm:ss.mmm with three-digit fractions distinguishes SBV from SubViewer.
  const std::size_t comma = first.find(',');
  if (comma == std::string_view::npos || !sbv_timing(first)) return 0;
  const std::size_t dot = first.find('.');
  return dot != std::string_view::npos && comma > dot && comma - dot == 4 ? 85 : 0;
}

// ============================================================================ SubViewer

Result<ir::Subtitles> parse_subviewer(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  const std::vector<std::string_view> lines = text_lines(text);
  bool version1 = false;
  for (const std::string_view line : lines) {
    if (line.find("**START SCRIPT**") != std::string_view::npos) version1 = true;
    if (util::istarts_with(util::trim(line), "[TITLE]")) {
      const std::string title(util::trim(util::trim(line).substr(7)));
      if (!title.empty()) subs.metadata["title"] = title;
    }
  }
  if (version1) {
    // [hh:mm:ss] start line, text lines, [hh:mm:ss] end line.
    std::optional<std::int64_t> start;
    std::vector<std::string> text_lines_seen;
    for (const std::string_view raw : lines) {
      const std::string_view line = util::trim(raw);
      if (line.size() >= 10 && line.front() == '[' && line.back() == ']' && util::is_ascii_digit(line[1])) {
        const auto time = parse_clock(line.substr(1, line.size() - 2), false);
        if (!time) continue;
        if (start) {
          ir::Cue cue;
          cue.start_ms = *start;
          cue.end_ms = *time;
          cue.text = html_to_ir(util::replace_all(join_lines(text_lines_seen), "|", "\n"));
          subs.cues.push_back(std::move(cue));
          start.reset();
        } else {
          start = *time;
        }
        text_lines_seen.clear();
      } else if (start && !line.empty()) {
        text_lines_seen.emplace_back(line);
      }
    }
  } else {
    std::size_t i = 0;
    while (i < lines.size()) {
      const std::string_view line = util::trim(lines[i]);
      const std::vector<std::string_view> parts = util::split(line, ',');
      std::optional<std::int64_t> start;
      std::optional<std::int64_t> end;
      if (parts.size() == 2 && !line.starts_with('[')) {
        start = parse_clock(parts[0]);
        end = parse_clock(parts[1]);
      }
      if (!start || !end) {
        ++i;
        continue;
      }
      std::size_t j = i + 1;
      while (j < lines.size() && !util::trim(lines[j]).empty()) ++j;
      ir::Cue cue;
      cue.start_ms = *start;
      cue.end_ms = *end;
      std::string body = body_text(std::span(lines).subspan(i + 1, j - i - 1));
      body = util::replace_all(body, "[br]", "\n");
      body = util::replace_all(body, "[BR]", "\n");
      cue.text = html_to_ir(body);
      subs.cues.push_back(std::move(cue));
      i = j;
    }
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no SubViewer time codes found");
  return subs;
}

Result<std::string> write_subviewer(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string title;
  if (const auto it = subs.metadata.find("title"); it != subs.metadata.end()) title = it->second;
  std::string out = "[INFORMATION]\n[TITLE]" + title +
                    "\n[AUTHOR]\n[SOURCE]\n[PRG]\n[FILEPATH]\n[DELAY]0\n[CD TRACK]0\n[COMMENT]\n[END INFORMATION]\n"
                    "[SUBTITLE]\n[COLF]&HFFFFFF,[STYLE]bd,[SIZE]18,[FONT]Arial\n";
  bool dropped = false;
  const ClockStyle style{2, '.', 2};
  for (const ir::Cue& cue : subs.cues) {
    const std::string plain = strip_markup(cue.text);
    dropped = dropped || plain != cue.text;
    out += format_clock(cue.start_ms, style) + "," + format_clock(cue.end_ms, style) + "\n";
    out += util::replace_all(plain, "\n", "[br]") + "\n\n";
  }
  if (dropped) ctx.warn("SubViewer cannot mark italic, bold or underlined text; that formatting was removed.");
  return out;
}

int sniff_subviewer(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 1024);
  if (head.starts_with("[INFORMATION]") || head.find("**START SCRIPT**") != std::string::npos) return 90;
  const std::vector<std::string_view> lines = text_lines(head);
  if (lines.empty()) return 0;
  const std::string_view first = util::trim(lines[0]);
  // hh:mm:ss.cc,hh:mm:ss.cc
  if (first.size() == 23 && first[2] == ':' && first[8] == '.' && first[11] == ',' && first[13] == ':' &&
      first[19] == '.') {
    return 80;
  }
  return 0;
}

// ============================================================================ MicroDVD

double microdvd_read_fps(const ReadContext& ctx) { return ctx.options.get_float("fps", 0.0, 0.0, 1000.0); }

/// "{y:i}" style control codes; returns the text without them and applies
/// italic / bold / underline per line ("y") or to every line ("Y").
std::string microdvd_text_to_ir(std::string_view text) {
  std::vector<std::string> lines;
  std::string all_open;
  std::string all_close;
  std::vector<std::string_view> raw_lines = util::split(text, '|');
  for (std::string_view line : raw_lines) {
    std::string open;
    std::string close;
    std::string body;
    std::size_t i = 0;
    line = util::trim(line);
    if (line.starts_with('/')) {
      open += "<i>";
      close.insert(0, "</i>");
      line.remove_prefix(1);
    }
    while (i < line.size()) {
      if (line[i] == '{' && i + 2 < line.size() && line[i + 2] == ':') {
        const std::size_t end = line.find('}', i);
        if (end != std::string_view::npos) {
          const char code = line[i + 1];
          const std::string value = util::to_lower(line.substr(i + 3, end - i - 3));
          if (code == 'y' || code == 'Y') {
            std::string& o = code == 'Y' ? all_open : open;
            std::string& c = code == 'Y' ? all_close : close;
            for (const char style : value) {
              if (style == 'i' || style == 'b' || style == 'u') {
                o += std::format("<{}>", style);
                c.insert(0, std::format("</{}>", style));
              }
            }
          }
          i = end + 1;
          continue;
        }
      }
      body.push_back(line[i++]);
    }
    lines.push_back(open + body + close);
  }
  return html_to_ir(all_open + join_lines(lines) + all_close);
}

Result<ir::Subtitles> parse_microdvd(std::string_view text, const ReadContext& ctx) {
  ir::Subtitles subs;
  double fps = microdvd_read_fps(ctx);
  std::vector<bool> open_end;
  bool first_cue = true;
  for (const std::string_view raw : text_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (!line.starts_with('{')) continue;
    const std::size_t close1 = line.find('}');
    if (close1 == std::string_view::npos || close1 + 1 >= line.size() || line[close1 + 1] != '{') continue;
    const std::size_t close2 = line.find('}', close1 + 1);
    if (close2 == std::string_view::npos) continue;
    const auto start = util::parse_int(line.substr(1, close1 - 1));
    const std::string_view end_text = line.substr(close1 + 2, close2 - close1 - 2);
    const auto end = end_text.empty() ? std::optional<std::int64_t>(-1) : util::parse_int(end_text);
    if (!start || !end || *start < 0 || *start > 1'000'000'000 || *end > 1'000'000'000) continue;
    const std::string_view body = line.substr(close2 + 1);
    // "{1}{1}23.976": the frame rate of the file.
    if (first_cue && *start <= 1 && *end <= 1) {
      if (const auto rate = util::parse_double(util::trim(body)); rate && *rate > 0 && *rate < 1000) {
        if (fps <= 0.0) fps = *rate;
        first_cue = false;
        continue;
      }
    }
    first_cue = false;
    ir::Cue cue;
    cue.start_ms = *start;  // frames for now
    cue.end_ms = *end;
    cue.text = microdvd_text_to_ir(body);
    open_end.push_back(*end < 0);
    subs.cues.push_back(std::move(cue));
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no MicroDVD '{frame}{frame}' lines found");
  if (fps <= 0.0) fps = 23.976;
  for (ir::Cue& cue : subs.cues) {
    cue.start_ms = frames_to_ms(cue.start_ms, fps);
    cue.end_ms = cue.end_ms < 0 ? cue.start_ms : frames_to_ms(cue.end_ms, fps);
  }
  fill_open_ends(subs.cues, open_end);
  subs.metadata["fps"] = util::format_double(fps);
  return subs;
}

Result<std::string> write_microdvd(const ir::Subtitles& subs, const WriteContext& ctx) {
  double fps = ctx.options.get_float("fps", 0.0, 0.0, 1000.0);
  if (fps <= 0.0) {
    const auto it = subs.metadata.find("fps");
    fps = it != subs.metadata.end() ? util::parse_double(it->second).value_or(23.976) : 23.976;
    if (fps <= 0.0 || fps >= 1000.0) fps = 23.976;
  }
  std::string out;
  if (ctx.options.get_bool("microdvd_header", true)) out += "{1}{1}" + util::format_double(fps) + "\n";
  bool dropped = false;
  for (const ir::Cue& cue : subs.cues) {
    std::string body = ir_to_pipe_text(cue.text, "{y:i}", &dropped);
    out += std::format("{{{}}}{{{}}}{}\n", ms_to_frames(cue.start_ms, fps), ms_to_frames(cue.end_ms, fps), body);
  }
  if (dropped) ctx.warn("MicroDVD marks formatting per line; partial-line formatting was removed.");
  return out;
}

int sniff_microdvd(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 512);
  const std::string_view line = util::trim(text_lines(head).empty() ? std::string_view() : text_lines(head)[0]);
  if (!line.starts_with('{')) return 0;
  const std::size_t close1 = line.find('}');
  if (close1 == std::string_view::npos || close1 < 2 || close1 + 1 >= line.size() || line[close1 + 1] != '{') return 0;
  return all_digits(line.substr(1, close1 - 1)) ? 90 : 0;
}

// ============================================================================ MPL2

Result<ir::Subtitles> parse_mpl2(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  std::vector<bool> open_end;
  for (const std::string_view raw : text_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (!line.starts_with('[')) continue;
    const std::size_t close1 = line.find(']');
    if (close1 == std::string_view::npos || close1 + 1 >= line.size() || line[close1 + 1] != '[') continue;
    const std::size_t close2 = line.find(']', close1 + 1);
    if (close2 == std::string_view::npos) continue;
    const auto start = util::parse_int(line.substr(1, close1 - 1));
    const std::string_view end_text = line.substr(close1 + 2, close2 - close1 - 2);
    const auto end = end_text.empty() ? std::optional<std::int64_t>(-1) : util::parse_int(end_text);
    if (!start || !end || *start < 0 || *start > 100'000'000 || *end > 100'000'000) continue;
    ir::Cue cue;
    cue.start_ms = *start * 100;
    cue.end_ms = *end < 0 ? cue.start_ms : *end * 100;
    cue.text = pipe_text_to_ir(line.substr(close2 + 1), true);
    open_end.push_back(*end < 0);
    subs.cues.push_back(std::move(cue));
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no MPL2 '[start][end]' lines found");
  fill_open_ends(subs.cues, open_end);
  return subs;
}

Result<std::string> write_mpl2(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string out;
  bool dropped = false;
  for (const ir::Cue& cue : subs.cues) {
    out += std::format("[{}][{}]{}\n", (cue.start_ms + 50) / 100, (cue.end_ms + 50) / 100,
                       ir_to_pipe_text(cue.text, "/", &dropped));
  }
  if (dropped) ctx.warn("MPL2 marks italics per line; other formatting was removed.");
  return out;
}

int sniff_mpl2(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 512);
  const std::vector<std::string_view> lines = text_lines(head);
  if (lines.empty()) return 0;
  const std::string_view line = util::trim(lines[0]);
  const std::size_t close1 = line.find(']');
  if (!line.starts_with('[') || close1 == std::string_view::npos || close1 < 2 || close1 + 1 >= line.size() ||
      line[close1 + 1] != '[') {
    return 0;
  }
  return all_digits(line.substr(1, close1 - 1)) ? 88 : 0;
}

// ============================================================================ TMPlayer

/// "00:00:01:text", "0:00:01=text", "00:00:01,1=text" -> (time, text).
std::optional<std::pair<std::int64_t, std::string_view>> tmplayer_line(std::string_view line) {
  std::size_t i = 0;
  int fields = 0;
  std::int64_t total = 0;
  std::int64_t field = 0;
  std::size_t digits = 0;
  while (i < line.size() && fields < 3) {
    const char c = line[i];
    if (util::is_ascii_digit(c)) {
      field = field * 10 + (c - '0');
      if (++digits > 2) return std::nullopt;
      ++i;
      continue;
    }
    if (digits == 0) return std::nullopt;
    total = total * 60 + field;
    ++fields;
    field = 0;
    digits = 0;
    if (fields < 3 && c != ':') return std::nullopt;
    if (fields == 3) {
      // ",1=" variant: a line number after a comma.
      if (c == ',') {
        const std::size_t eq = line.find('=', i);
        if (eq == std::string_view::npos) return std::nullopt;
        i = eq;
      }
      if (line[i] != ':' && line[i] != '=') return std::nullopt;
      return std::pair{total * 1000, line.substr(i + 1)};
    }
    ++i;
  }
  return std::nullopt;
}

Result<ir::Subtitles> parse_tmplayer(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  std::vector<bool> open_end;
  for (const std::string_view raw : text_lines(text)) {
    const auto parsed = tmplayer_line(util::trim(raw));
    if (!parsed) continue;
    ir::Cue cue;
    cue.start_ms = parsed->first;
    cue.end_ms = parsed->first;
    cue.text = pipe_text_to_ir(parsed->second, true);
    open_end.push_back(true);
    subs.cues.push_back(std::move(cue));
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no TMPlayer 'hh:mm:ss:' lines found");
  // Empty lines only end the cue before them; fill_open_ends treats them as
  // cues (they are dropped later as empty).
  fill_open_ends(subs.cues, open_end);
  return subs;
}

Result<std::string> write_tmplayer(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string out;
  bool dropped = false;
  const ClockStyle style{2, ',', 0};
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    const ir::Cue& cue = subs.cues[i];
    const std::int64_t start_s = cue.start_ms / 1000;
    out += format_clock(start_s * 1000, style) + ":" + ir_to_pipe_text(cue.text, "/", &dropped) + "\n";
    // TMPlayer has no end times: an empty line clears the screen.
    const std::int64_t end_s = (cue.end_ms + 999) / 1000;
    const bool next_later = i + 1 >= subs.cues.size() || subs.cues[i + 1].start_ms / 1000 > end_s;
    if (next_later && end_s > start_s) out += format_clock(end_s * 1000, style) + ":\n";
  }
  if (dropped) ctx.warn("TMPlayer marks italics per line; other formatting was removed.");
  ctx.warn("TMPlayer stores whole seconds; times were rounded.");
  return out;
}

int sniff_tmplayer(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 512);
  const std::vector<std::string_view> lines = text_lines(head);
  if (lines.empty()) return 0;
  return tmplayer_line(util::trim(lines[0])) ? 75 : 0;
}

// ============================================================================ QuickTime Text

Result<ir::Subtitles> parse_qttext(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  double time_scale = 0.0;
  if (const std::size_t at = text.find("{timeScale:"); at != std::string_view::npos) {
    const std::size_t end = text.find('}', at);
    if (end != std::string_view::npos) time_scale = util::parse_double(util::trim(text.substr(at + 11, end - at - 11))).value_or(0.0);
  }
  std::vector<std::pair<std::int64_t, std::vector<std::string>>> stamps;
  for (const std::string_view raw : text_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.size() >= 10 && line.front() == '[' && line.back() == ']' && util::is_ascii_digit(line[1])) {
      std::string_view stamp = line.substr(1, line.size() - 2);
      const std::size_t dot = stamp.rfind('.');
      std::int64_t ms = 0;
      if (dot != std::string_view::npos && time_scale > 0.0) {
        const auto whole = parse_clock(stamp.substr(0, dot), false);
        const auto units = util::parse_int(stamp.substr(dot + 1));
        if (!whole || !units) continue;
        ms = *whole + std::llround(static_cast<double>(*units) * 1000.0 / time_scale);
      } else {
        const auto parsed = parse_clock(stamp, false);
        if (!parsed) continue;
        ms = *parsed;
      }
      stamps.push_back({ms, {}});
      continue;
    }
    if (line.starts_with('{') || stamps.empty()) continue;  // header descriptors
    if (!line.empty()) stamps.back().second.emplace_back(line);
  }
  for (std::size_t i = 0; i < stamps.size(); ++i) {
    if (stamps[i].second.empty()) continue;
    ir::Cue cue;
    cue.start_ms = stamps[i].first;
    cue.end_ms = i + 1 < stamps.size() ? stamps[i + 1].first : stamps[i].first + 2000;
    cue.text = html_to_ir(join_lines(stamps[i].second));
    subs.cues.push_back(std::move(cue));
  }
  if (stamps.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no QuickTime Text time stamps found");
  return subs;
}

Result<std::string> write_qttext(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string out =
      "{QTtext} {font:Helvetica}\n{plain} {size:24}\n{timeScale:1000}\n{width:640} {height:64}\n"
      "{timestamps:absolute} {justify:center}\n";
  bool dropped = false;
  const ClockStyle style{2, '.', 3};
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    const ir::Cue& cue = subs.cues[i];
    const std::string plain = strip_markup(cue.text);
    dropped = dropped || plain != cue.text;
    out += "[" + format_clock(cue.start_ms, style) + "]\n" + plain + "\n";
    if (i + 1 >= subs.cues.size() || subs.cues[i + 1].start_ms > cue.end_ms) {
      out += "[" + format_clock(cue.end_ms, style) + "]\n\n";
    }
  }
  if (dropped) ctx.warn("QuickTime Text formatting is per file; italic, bold and underline marks were removed.");
  return out;
}

int sniff_qttext(ByteView data, std::string_view /*filename*/) {
  return sniff_prefix(data, 64).starts_with("{QTtext}") ? 95 : 0;
}

// ============================================================================ Spruce STL

double spruce_fps(const Options& options) {
  const double fps = options.get_float("fps", 0.0, 0.0, 1000.0);
  return fps > 0.0 ? fps : 25.0;
}

std::optional<std::int64_t> frame_clock(std::string_view text, double fps) {
  const std::vector<std::string_view> parts = util::split(util::trim(text), ':');
  if (parts.size() != 4) return std::nullopt;
  std::int64_t values[4] = {0, 0, 0, 0};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto value = util::parse_int(util::trim(parts[i]));
    if (!value || *value < 0 || *value > 100000) return std::nullopt;
    values[i] = *value;
  }
  return ((values[0] * 60 + values[1]) * 60 + values[2]) * 1000 + frames_to_ms(values[3], fps);
}

std::string format_frame_clock(std::int64_t ms, double fps) {
  std::int64_t frames = ms_to_frames(ms, fps);
  const auto per_second = static_cast<std::int64_t>(std::llround(fps));
  const std::int64_t fps_whole = per_second > 0 ? per_second : 25;
  // Express in whole seconds + frames of the nominal rate.
  const std::int64_t total_seconds = ms / 1000;
  frames = ms_to_frames(ms - total_seconds * 1000, fps);
  if (frames >= fps_whole) frames = fps_whole - 1;
  return std::format("{:02}:{:02}:{:02}:{:02}", total_seconds / 3600, (total_seconds / 60) % 60, total_seconds % 60, frames);
}

std::string spruce_text_to_ir(std::string_view text) {
  std::string out;
  bool italic = false, bold = false, underline = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '^' && i + 1 < text.size()) {
      const char code = text[i + 1];
      bool* flag = code == 'I' ? &italic : code == 'B' ? &bold : code == 'U' ? &underline : nullptr;
      if (flag != nullptr) {
        *flag = !*flag;
        const char tag = static_cast<char>(util::ascii_lower(code));
        out += *flag ? std::format("<{}>", tag) : std::format("</{}>", tag);
        ++i;
        continue;
      }
    }
    out.push_back(text[i] == '|' ? '\n' : text[i]);
  }
  return balance_markup(out);
}

Result<ir::Subtitles> parse_spruce(std::string_view text, const ReadContext& ctx) {
  ir::Subtitles subs;
  const double fps = spruce_fps(ctx.options);
  for (const std::string_view raw : text_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.empty() || line.starts_with('$') || line.starts_with("//")) continue;
    const std::size_t comma1 = line.find(',');
    if (comma1 == std::string_view::npos) continue;
    const std::size_t comma2 = line.find(',', comma1 + 1);
    if (comma2 == std::string_view::npos) continue;
    const auto start = frame_clock(line.substr(0, comma1), fps);
    const auto end = frame_clock(line.substr(comma1 + 1, comma2 - comma1 - 1), fps);
    if (!start || !end) continue;
    ir::Cue cue;
    cue.start_ms = *start;
    cue.end_ms = *end;
    cue.text = spruce_text_to_ir(util::trim(line.substr(comma2 + 1)));
    subs.cues.push_back(std::move(cue));
  }
  if (subs.cues.empty() && !util::trim(text).empty()) return fail(Status::Decode, "no Spruce STL time codes found");
  return subs;
}

Result<std::string> write_spruce(const ir::Subtitles& subs, const WriteContext& ctx) {
  const double fps = spruce_fps(ctx.options);
  std::string out = "$FontName = Arial\n$FontSize = 42\n$HorzAlign = Center\n$VertAlign = Bottom\n$TapeOffset = FALSE\n\n";
  for (const ir::Cue& cue : subs.cues) {
    std::string body;
    for (const MarkupToken& token : tokenize_markup(cue.text)) {
      switch (token.type) {
        case MarkupToken::Type::Text: body += util::replace_all(token.text, "|", "/"); break;
        case MarkupToken::Type::LineBreak: body += "|"; break;
        case MarkupToken::Type::Open:
        case MarkupToken::Type::Close: body += "^" + util::to_upper(token.text); break;
      }
    }
    out += format_frame_clock(cue.start_ms, fps) + " , " + format_frame_clock(cue.end_ms, fps) + " , " + body + "\n";
  }
  return out;
}

int sniff_spruce(ByteView data, std::string_view /*filename*/) {
  if (!util::looks_like_text(data.first(std::min<std::size_t>(data.size(), 512)))) return 0;
  const std::string head = sniff_prefix(data, 1024);
  for (const std::string_view raw : text_lines(head)) {
    const std::string_view line = util::trim(raw);
    if (line.empty() || line.starts_with("//")) continue;
    if (line.starts_with("$FontName") || line.starts_with("$TapeOffset") || line.starts_with("$FontSize") ||
        line.starts_with("$HorzAlign") || line.starts_with("$VertAlign")) {
      return 90;
    }
    const std::size_t comma1 = line.find(',');
    if (comma1 == std::string_view::npos) return 0;
    const std::size_t comma2 = line.find(',', comma1 + 1);
    if (comma2 == std::string_view::npos) return 0;
    return frame_clock(line.substr(0, comma1), 25.0) && frame_clock(line.substr(comma1 + 1, comma2 - comma1 - 1), 25.0) ? 85 : 0;
  }
  return 0;
}

// ============================================================================ LRC

/// "[mm:ss.xx]" -> ms; also [mm:ss], [mm:ss:xx], [h:mm:ss.xx].
std::optional<std::int64_t> lrc_stamp(std::string_view inside) {
  if (inside.empty() || !util::is_ascii_digit(inside[0])) return std::nullopt;
  const std::size_t colons = static_cast<std::size_t>(std::ranges::count(inside, ':'));
  if (colons == 2 && inside.find('.') == std::string_view::npos) {
    // mm:ss:xx (centiseconds after a colon)
    const std::size_t last = inside.rfind(':');
    std::string fixed(inside);
    fixed[last] = '.';
    return parse_clock(fixed, true);
  }
  return parse_clock(inside, true);
}

/// Remove enhanced-LRC word time stamps "<mm:ss.xx>".
std::string strip_word_stamps(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '<') {
      const std::size_t close = text.find('>', i);
      if (close != std::string_view::npos && lrc_stamp(text.substr(i + 1, close - i - 1))) {
        i = close + 1;
        continue;
      }
    }
    out.push_back(text[i++]);
  }
  return out;
}

Result<ir::Subtitles> parse_lrc(std::string_view text, const ReadContext& /*ctx*/) {
  ir::Subtitles subs;
  std::int64_t offset = 0;
  std::optional<std::int64_t> length;
  struct Line {
    std::int64_t time;
    std::string text;
  };
  std::vector<Line> timed;
  for (const std::string_view raw : text_lines(text)) {
    std::string_view line = util::trim(raw);
    std::vector<std::int64_t> times;
    while (line.starts_with('[')) {
      const std::size_t close = line.find(']');
      if (close == std::string_view::npos) break;
      const std::string_view inside = line.substr(1, close - 1);
      if (const auto time = lrc_stamp(inside)) {
        times.push_back(*time);
      } else if (const std::size_t colon = inside.find(':'); colon != std::string_view::npos && times.empty()) {
        const std::string key = util::to_lower(util::trim(inside.substr(0, colon)));
        const std::string value(util::trim(inside.substr(colon + 1)));
        if (key == "offset") offset = util::parse_int(value).value_or(0);
        else if (key == "length") length = parse_clock(value, true);
        else if (key == "ti") subs.metadata["title"] = value;
        else if (key == "ar") subs.metadata["artist"] = value;
        else if (key == "al") subs.metadata["album"] = value;
        else if (key == "au") subs.metadata["author"] = value;
        else if (key == "la" || key == "lang") subs.metadata["language"] = value;
        else if (!key.empty()) subs.metadata["lrc." + key] = value;
      } else {
        break;
      }
      line.remove_prefix(close + 1);
    }
    const std::string body = strip_word_stamps(util::trim(line));
    for (const std::int64_t time : times) timed.push_back({time - std::clamp<std::int64_t>(offset, -86'400'000, 86'400'000), body});
  }
  if (timed.empty() && !util::trim(text).empty() && subs.metadata.empty()) {
    return fail(Status::Decode, "no LRC '[mm:ss.xx]' time stamps found");
  }
  std::ranges::stable_sort(timed, [](const Line& a, const Line& b) { return a.time < b.time; });
  // Lines sharing a time stamp form one cue (multi-line cues written as
  // several lines with the same stamp).
  std::size_t i = 0;
  while (i < timed.size()) {
    std::size_t j = i;
    std::vector<std::string> texts;
    while (j < timed.size() && timed[j].time == timed[i].time) {
      if (!util::trim(timed[j].text).empty()) texts.push_back(timed[j].text);
      ++j;
    }
    if (!texts.empty()) {
      ir::Cue cue;
      cue.start_ms = std::max<std::int64_t>(timed[i].time, 0);
      std::int64_t end = j < timed.size() ? timed[j].time : cue.start_ms + 5000;
      if (j == timed.size() && length && *length > cue.start_ms) end = *length;
      cue.end_ms = end;
      cue.text = html_to_ir(join_lines(texts));
      subs.cues.push_back(std::move(cue));
    }
    i = j;
  }
  return subs;
}

std::string lrc_clock(std::int64_t ms) {
  ms = std::max<std::int64_t>(ms, 0);
  std::int64_t centis = (ms + 5) / 10;
  return std::format("[{:02}:{:02}.{:02}]", centis / 6000, (centis / 100) % 60, centis % 100);
}

Result<std::string> write_lrc(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string out;
  for (const auto& [key, tag] : {std::pair{"title"sv, "ti"sv}, std::pair{"artist"sv, "ar"sv}, std::pair{"album"sv, "al"sv},
                                 std::pair{"author"sv, "au"sv}, std::pair{"language"sv, "la"sv}}) {
    if (const auto it = subs.metadata.find(std::string(key)); it != subs.metadata.end() && !it->second.empty()) {
      out += std::format("[{}:{}]\n", tag, util::replace_all(it->second, "]", ")"));
    }
  }
  bool dropped = false;
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    const ir::Cue& cue = subs.cues[i];
    const std::string plain = strip_markup(cue.text);
    dropped = dropped || plain != cue.text;
    // Each line of a multi-line cue gets the cue's time stamp.
    for (const std::string_view line : util::split(plain, '\n', false)) out += lrc_clock(cue.start_ms) + std::string(line) + "\n";
    if (i + 1 >= subs.cues.size() || subs.cues[i + 1].start_ms > cue.end_ms + 10) out += lrc_clock(cue.end_ms) + "\n";
  }
  if (dropped) ctx.warn("LRC lyrics are plain text; italic, bold and underline marks were removed.");
  return out;
}

int sniff_lrc(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 1024);
  int tags = 0;
  for (const std::string_view raw : text_lines(head)) {
    const std::string_view line = util::trim(raw);
    if (line.empty()) continue;
    if (!line.starts_with('[')) return 0;
    const std::size_t close = line.find(']');
    if (close == std::string_view::npos) return 0;
    const std::string_view inside = line.substr(1, close - 1);
    if (lrc_stamp(inside) && inside.find(':') != std::string_view::npos) return 88;
    const std::size_t colon = inside.find(':');
    if (colon == std::string_view::npos || colon > 6 || ++tags > 12) return 0;
  }
  return tags > 0 ? 60 : 0;
}

// ============================================================================ registration

void add(Registry& registry, FormatInfo info, std::function<int(ByteView, std::string_view)> sniff, Codec codec) {
  info.sniff = std::move(sniff);
  registry.add_format(std::move(info));
  registry.add_codec(std::move(codec));
}

OptionSpec fps_option(double default_fps, std::string description) {
  return OptionSpec::number("fps", "Frame rate", default_fps, 0.0, 1000.0, std::move(description));
}

}  // namespace

void register_text_formats(Registry& registry) {
  add(registry,
      subtitle_format("srt", "SubRip subtitles", "The most widely supported plain-text subtitle format.", {"srt"},
                      "application/x-subrip", 85),
      sniff_srt, make_text_codec("srt", parse_srt, write_srt));

  {
    FormatInfo info = subtitle_format("vtt", "WebVTT subtitles", "W3C Web Video Text Tracks, the subtitle format of HTML video.",
                                      {"vtt"}, "text/vtt", 75);
    info.signatures = {Signature::at(0, "'WEBVTT'"), Signature::at(3, "'WEBVTT'")};
    add(registry, std::move(info), sniff_vtt, make_text_codec("vtt", parse_vtt, write_vtt));
  }

  add(registry,
      subtitle_format("sbv", "YouTube SBV captions", "SubViewer-style captions as exported by YouTube.", {"sbv"},
                      "text/plain", 25),
      sniff_sbv, make_text_codec("sbv", parse_sbv, write_sbv));

  add(registry,
      subtitle_format("subviewer", "SubViewer subtitles", "SubViewer 1.0 and 2.0 text subtitles.", {"sub"},
                      "text/plain", 20),
      sniff_subviewer, make_text_codec("subviewer", parse_subviewer, write_subviewer, {}, {}, "crlf"));

  add(registry,
      subtitle_format("microdvd", "MicroDVD subtitles", "Frame-based subtitles ({start}{end}text).", {"sub", "txt"},
                      "text/x-microdvd", 30),
      sniff_microdvd,
      make_text_codec("microdvd", parse_microdvd, write_microdvd,
                      {fps_option(0.0, "Frame rate the frame numbers refer to; 0 = the rate stored in the file, else 23.976.")},
                      {fps_option(0.0, "Frame rate for the frame numbers; 0 = the source's rate, else 23.976."),
                       OptionSpec::boolean("microdvd_header", "Write frame-rate line", true,
                                           "Start the file with the {1}{1}fps line that players use to detect the rate.")
                           .advanced()}));

  add(registry,
      subtitle_format("mpl2", "MPL2 subtitles", "Decisecond-timed subtitles ([start][end]text).", {"mpl", "txt"},
                      "text/plain", 10),
      sniff_mpl2, make_text_codec("mpl2", parse_mpl2, write_mpl2));

  add(registry,
      subtitle_format("tmplayer", "TMPlayer subtitles", "Start-time-only subtitles (hh:mm:ss:text).", {"tmp", "txt"},
                      "text/plain", 10),
      sniff_tmplayer, make_text_codec("tmplayer", parse_tmplayer, write_tmplayer));

  add(registry,
      subtitle_format("qttext", "QuickTime Text", "Apple QuickTime text track descriptions ({QTtext}).",
                      {"txt", "qt.txt"}, "text/plain", 10),
      sniff_qttext, make_text_codec("qttext", parse_qttext, write_qttext));

  add(registry,
      subtitle_format("spruce-stl", "Spruce STL subtitles", "Spruce / DVD Studio Pro text subtitles (hh:mm:ss:ff , ... , text).",
                      {"stl"}, "text/plain", 12),
      sniff_spruce,
      make_text_codec("spruce-stl", parse_spruce, write_spruce,
                      {fps_option(0.0, "Frame rate of the time codes' frame field; 0 = 25.")},
                      {fps_option(0.0, "Frame rate of the time codes' frame field; 0 = 25.")}));

  add(registry,
      subtitle_format("lrc", "LRC lyrics", "Time-tagged song lyrics ([mm:ss.xx]line), including enhanced word timing.",
                      {"lrc"}, "application/x-lrc", 45),
      sniff_lrc, make_text_codec("lrc", parse_lrc, write_lrc));
}

}  // namespace cvt::subtitle
