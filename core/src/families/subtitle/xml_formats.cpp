// Convertible core - subtitle family: XML and HTML-like subtitle formats.
//
//   ttml      W3C Timed Text (TTML1/TTML2), also the IMSC1 text profile (writer option)
//   dfxp      Distribution Format Exchange Profile (TTML1 under its old name and namespace)
//   ebu-tt    EBU-TT / EBU-TT-D (Tech 3350 / 3380); written as EBU-TT-D
//   smpte-tt  SMPTE ST 2052-1 timed text (read; image subtitles are skipped)
//   sami      Microsoft SAMI (.smi), HTML-like, several languages in one file
//   srv3      YouTube timed text format 3
//
// TTML timing follows the specification: begin/end/dur on body, div, p and
// span with par semantics (children are relative to their parent's begin),
// every time-expression form (clock times with fractions or frames and
// sub-frames, offset times in h/m/s/ms/f/t), ttp:frameRate,
// frameRateMultiplier, subFrameRate and tickRate. Styling basics (italic,
// bold, underline; text and display alignment) are resolved through style
// references, regions and inheritance.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <tuple>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/subtitle/subtitle.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::subtitle {

namespace {

using namespace std::string_view_literals;
using util::XmlNode;

constexpr std::string_view ttml_ns = "http://www.w3.org/ns/ttml";
constexpr std::string_view ttml_styling_ns = "http://www.w3.org/ns/ttml#styling";
constexpr std::string_view ttml_parameter_ns = "http://www.w3.org/ns/ttml#parameter";
constexpr std::string_view ttml_metadata_ns = "http://www.w3.org/ns/ttml#metadata";
constexpr int max_depth = 64;

// ============================================================================ TTML timing

struct TimeParams {
  double frame_rate = 30.0;  ///< effective (multiplier applied)
  double sub_frame_rate = 1.0;
  double tick_rate = 1.0;
};

std::optional<std::int64_t> parse_ttml_time(std::string_view text, const TimeParams& params) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  if (text.find(':') != std::string_view::npos) {
    const std::vector<std::string_view> parts = util::split(text, ':');
    if (parts.size() == 3) return parse_clock(text, false);
    if (parts.size() != 4) return std::nullopt;
    // hh:mm:ss:frames(.subframes)
    const auto base = parse_clock(std::string(parts[0]) + ":" + std::string(parts[1]) + ":" + std::string(parts[2]), false);
    if (!base) return std::nullopt;
    const std::vector<std::string_view> frame_parts = util::split(parts[3], '.');
    const auto frames = util::parse_double(frame_parts[0]);
    const auto sub = frame_parts.size() > 1 ? util::parse_double(frame_parts[1]) : std::optional<double>(0.0);
    if (!frames || !sub || *frames < 0 || *frames > 1e6 || *sub < 0) return std::nullopt;
    const double seconds = (*frames + *sub / std::max(params.sub_frame_rate, 1.0)) / params.frame_rate;
    return *base + std::llround(seconds * 1000.0);
  }
  // Offset time: number followed by a metric.
  std::size_t split = text.size();
  while (split > 0 && util::is_ascii_alpha(text[split - 1])) --split;
  const std::string_view metric = text.substr(split);
  const auto value = util::parse_double(text.substr(0, split));
  if (!value || !std::isfinite(*value) || *value < 0 || *value > 1e12) return std::nullopt;
  double ms = 0.0;
  if (metric == "h") ms = *value * 3'600'000.0;
  else if (metric == "m") ms = *value * 60'000.0;
  else if (metric == "s" || metric.empty()) ms = *value * 1000.0;
  else if (metric == "ms") ms = *value;
  else if (metric == "f") ms = *value * 1000.0 / params.frame_rate;
  else if (metric == "t") ms = *value * 1000.0 / params.tick_rate;
  else return std::nullopt;
  if (ms > 4e12) return std::nullopt;
  return std::llround(ms);
}

TimeParams time_params(XmlNode root) {
  TimeParams params;
  if (const auto rate = util::parse_double(root.attribute_local("frameRate").value()); rate && *rate > 0 && *rate < 1000) {
    params.frame_rate = *rate;
  }
  const std::vector<std::string_view> multiplier = util::split(util::trim(root.attribute_local("frameRateMultiplier").value()), ' ', false);
  if (multiplier.size() == 2) {
    const auto num = util::parse_double(multiplier[0]);
    const auto den = util::parse_double(multiplier[1]);
    if (num && den && *num > 0 && *den > 0) params.frame_rate = params.frame_rate * *num / *den;
  }
  if (const auto sub = util::parse_double(root.attribute_local("subFrameRate").value()); sub && *sub >= 1 && *sub < 1000) {
    params.sub_frame_rate = *sub;
  }
  if (const auto tick = util::parse_double(root.attribute_local("tickRate").value()); tick && *tick > 0 && *tick < 1e9) {
    params.tick_rate = *tick;
  } else if (root.attribute_local("frameRate")) {
    params.tick_rate = params.frame_rate * params.sub_frame_rate;
  }
  return params;
}

// ============================================================================ TTML styling

struct Style {
  std::optional<bool> italic;
  std::optional<bool> bold;
  std::optional<bool> underline;
  std::optional<int> column;  ///< 1 left, 2 centre, 3 right
  std::optional<int> row;     ///< 0 bottom, 3 middle, 6 top

  void merge_over(const Style& base) {
    if (!italic) italic = base.italic;
    if (!bold) bold = base.bold;
    if (!underline) underline = base.underline;
    if (!column) column = base.column;
    if (!row) row = base.row;
  }
};

/// Styling attributes written directly on an element.
Style inline_style(XmlNode node) {
  Style style;
  for (const util::XmlAttribute attribute : node.attributes()) {
    const std::string_view name = attribute.local_name();
    const std::string_view value = util::trim(attribute.value());
    if (name == "fontStyle") style.italic = value == "italic" || value == "oblique";
    else if (name == "fontWeight") style.bold = value == "bold";
    else if (name == "textDecoration") {
      if (value.find("noUnderline") != std::string_view::npos) style.underline = false;
      else if (value.find("underline") != std::string_view::npos) style.underline = true;
    } else if (name == "textAlign") {
      if (value == "left" || value == "start") style.column = 1;
      else if (value == "right" || value == "end") style.column = 3;
      else if (value == "center") style.column = 2;
    } else if (name == "displayAlign") {
      if (value == "before") style.row = 6;
      else if (value == "center") style.row = 3;
      else if (value == "after") style.row = 0;
    }
  }
  return style;
}

struct Styling {
  std::map<std::string, XmlNode, std::less<>> style_nodes;
  std::map<std::string, Style, std::less<>> regions;

  /// A style with its own references resolved (depth-limited against cycles).
  Style resolve(std::string_view id, int depth = 0) const {
    const auto it = style_nodes.find(id);
    if (it == style_nodes.end() || depth > 16) return {};
    Style style = inline_style(it->second);
    for (const std::string_view ref : util::split(it->second.attribute_local("style").value(), ' ', false)) {
      style.merge_over(resolve(ref, depth + 1));
    }
    return style;
  }
  /// Inline attributes first, then referenced styles, then `inherited`.
  Style element_style(XmlNode node, const Style& inherited) const {
    Style style = inline_style(node);
    for (const std::string_view ref : util::split(node.attribute_local("style").value(), ' ', false)) {
      style.merge_over(resolve(ref));
    }
    style.merge_over(inherited);
    return style;
  }
};

Styling read_styling(XmlNode root) {
  Styling styling;
  const XmlNode head = root.child_local("head");
  for (const XmlNode style : head.child_local("styling").children_local("style")) {
    const std::string_view id = style.attribute_local("id").value();
    if (!id.empty()) styling.style_nodes.emplace(std::string(id), style);
  }
  for (const XmlNode region : head.child_local("layout").children_local("region")) {
    const std::string_view id = region.attribute_local("id").value();
    if (id.empty()) continue;
    Style style = inline_style(region);
    for (const XmlNode nested : region.children_local("style")) style.merge_over(inline_style(nested));
    for (const std::string_view ref : util::split(region.attribute_local("style").value(), ' ', false)) {
      style.merge_over(styling.resolve(ref));
    }
    // Without displayAlign, the region's vertical place on screen decides.
    const std::vector<std::string_view> origin = util::split(util::trim(region.attribute_local("origin").value()), ' ', false);
    const std::vector<std::string_view> extent = util::split(util::trim(region.attribute_local("extent").value()), ' ', false);
    if (origin.size() == 2 && origin[1].ends_with('%')) {
      const double y = util::parse_double(origin[1].substr(0, origin[1].size() - 1)).value_or(0.0) / 100.0;
      double h = 0.0;
      if (extent.size() == 2 && extent[1].ends_with('%')) {
        h = util::parse_double(extent[1].substr(0, extent[1].size() - 1)).value_or(0.0) / 100.0;
      }
      const int display = style.row.value_or(6);  // TTML's default displayAlign is "before"
      const double anchor = display == 6 ? y : display == 3 ? y + h / 2 : y + h;
      style.row = anchor < 0.34 ? 6 : anchor < 0.67 ? 3 : 0;
    }
    styling.regions.emplace(std::string(id), style);
  }
  return styling;
}

// ============================================================================ TTML body

struct Collector {
  const TimeParams& params;
  const Styling& styling;
  std::vector<ir::Cue>& cues;
  bool images = false;
};

/// Collapse XML white space (xml:space="default").
void append_text(std::string& out, std::string_view text, bool preserve) {
  if (preserve) {
    out.append(text);
    return;
  }
  for (const char c : text) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (space) {
      if (!out.empty() && out.back() != ' ' && out.back() != '\n') out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
}

std::string open_tags(const Style& style) {
  std::string out;
  if (style.underline.value_or(false)) out += "<u>";
  if (style.bold.value_or(false)) out += "<b>";
  if (style.italic.value_or(false)) out += "<i>";
  return out;
}
std::string close_tags(const Style& style) {
  std::string out;
  if (style.italic.value_or(false)) out += "</i>";
  if (style.bold.value_or(false)) out += "</b>";
  if (style.underline.value_or(false)) out += "</u>";
  return out;
}

/// The text of a paragraph's content with markup for the span styles.
void paragraph_text(XmlNode node, const Style& style, const Collector& c, std::string& out, bool preserve, int depth) {
  if (depth > max_depth) return;
  for (const XmlNode child : node.children()) {
    if (child.is_text()) {
      append_text(out, child.value(), preserve);
      continue;
    }
    if (!child.is_element()) continue;
    const std::string_view name = child.local_name();
    if (name == "br") {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out.push_back('\n');
    } else if (name == "span") {
      const Style span_style = c.styling.element_style(child, style);
      const std::string_view space = child.attribute_local("space").value();
      const bool keep = space == "preserve" || (preserve && space != "default");
      // Tags only for what differs from the paragraph.
      Style delta;
      if (span_style.italic.value_or(false) && !style.italic.value_or(false)) delta.italic = true;
      if (span_style.bold.value_or(false) && !style.bold.value_or(false)) delta.bold = true;
      if (span_style.underline.value_or(false) && !style.underline.value_or(false)) delta.underline = true;
      out += open_tags(delta);
      paragraph_text(child, span_style, c, out, keep, depth + 1);
      out += close_tags(delta);
    } else if (name != "metadata" && name != "set" && name != "animate") {
      paragraph_text(child, style, c, out, preserve, depth + 1);
    }
  }
}

struct Interval {
  std::int64_t begin = 0;
  std::optional<std::int64_t> end;  ///< nullopt = indefinite
};

Interval child_interval(XmlNode node, const Interval& parent, const TimeParams& params) {
  Interval out;
  const auto begin = parse_ttml_time(node.attribute_local("begin").value(), params);
  const auto end = parse_ttml_time(node.attribute_local("end").value(), params);
  const auto dur = parse_ttml_time(node.attribute_local("dur").value(), params);
  out.begin = parent.begin + begin.value_or(0);
  if (end) out.end = parent.begin + *end;
  if (dur) out.end = out.end ? std::min(*out.end, out.begin + *dur) : out.begin + *dur;
  if (parent.end) out.end = out.end ? std::min(*out.end, *parent.end) : *parent.end;
  return out;
}

bool has_timing(XmlNode node) {
  return node.attribute_local("begin") || node.attribute_local("end") || node.attribute_local("dur");
}

void emit(XmlNode node, const Interval& when, const Style& element_style, std::string_view region, const Collector& c,
          bool preserve) {
  // Content flowed into a region takes the region's alignment unless it sets its own.
  Style style = element_style;
  if (const auto it = c.styling.regions.find(region); it != c.styling.regions.end()) style.merge_over(it->second);
  std::string text;
  paragraph_text(node, style, c, text, preserve, 0);
  ir::Cue cue;
  cue.start_ms = when.begin;
  cue.end_ms = when.end.value_or(when.begin + 5000);  // indefinite: a sensible on-screen time
  // `text` already holds IR tags for the spans.
  cue.text = balance_markup(open_tags(style) + text + close_tags(style));
  const int row = style.row.value_or(0);
  const int column = style.column.value_or(2);
  if (row != 0 || column != 2) {
    ir::CuePosition position;
    position.alignment = row + column;
    cue.position = position;
  }
  if (!region.empty()) cue.style = std::string(region);
  if (const std::string_view agent = node.attribute_local("agent").value(); !agent.empty()) cue.speaker = std::string(agent);
  c.cues.push_back(std::move(cue));
}

void walk(XmlNode node, const Interval& parent, const Style& inherited, std::string_view region, Collector& c,
          bool preserve, int depth) {
  if (depth > max_depth) return;
  for (const XmlNode child : node.children()) {
    if (!child.is_element()) continue;
    const std::string_view name = child.local_name();
    if (name == "metadata" || name == "set" || name == "animation") continue;
    if (name == "image" || name == "backgroundImage") {
      c.images = true;
      continue;
    }
    const Interval when = child_interval(child, parent, c.params);
    std::string_view child_region = child.attribute_local("region").value();
    if (child_region.empty()) child_region = region;
    const Style style = c.styling.element_style(child, inherited);
    const std::string_view space = child.attribute_local("space").value();
    const bool keep = space == "preserve" || (preserve && space != "default");
    if (name == "p") {
      // Spans with their own timing in an untimed paragraph are separate cues.
      bool timed_spans = false;
      if (!has_timing(child) && !parent.end) {
        for (const XmlNode span : child.children_local("span")) timed_spans = timed_spans || has_timing(span);
      }
      if (timed_spans) {
        walk(child, when, style, child_region, c, keep, depth + 1);
      } else {
        emit(child, when, style, child_region, c, keep);
      }
    } else if (name == "span" && has_timing(child)) {
      emit(child, when, style, child_region, c, keep);
    } else {
      walk(child, when, style, child_region, c, keep, depth + 1);
    }
  }
}

Result<ir::Subtitles> parse_ttml_document(ByteView input, const ReadContext& ctx) {
  util::XmlParseOptions options;
  options.preserve_whitespace = true;
  options.replace_invalid_text = true;
  options.max_depth = 256;
  auto parsed = util::parse_xml(input, options);
  if (!parsed) return fail(parsed.error().code, "not a timed-text document: {}", parsed.error().message);
  const XmlNode root = parsed->root();
  if (root.local_name() != "tt") return fail(Status::Decode, "not a TTML document (the root element is <{}>)", root.local_name());
  ir::Subtitles subs;
  const TimeParams params = time_params(root);
  const Styling styling = read_styling(root);
  if (const std::string_view language = root.attribute_local("lang").value(); !language.empty()) {
    subs.metadata["language"] = std::string(language);
  }
  const XmlNode head_metadata = root.child_local("head").child_local("metadata");
  if (const std::string title(util::trim(head_metadata.child_local("title").text())); !title.empty()) {
    subs.metadata["title"] = title;
  }
  Collector collector{params, styling, subs.cues};
  const XmlNode body = root.child_local("body");
  if (body) {
    const Interval when = child_interval(body, Interval{}, params);
    const Style style = styling.element_style(body, {});
    const std::string_view region = body.attribute_local("region").value();
    walk(body, when, style, region, collector, body.attribute_local("space").value() == "preserve", 0);
  }
  if (collector.images) ctx.warn("Image subtitles in the timed-text file were skipped; only text is converted.");
  return subs;
}

// ---------------------------------------------------------------------------- TTML writing

enum class Profile { Ttml, Imsc1, EbuTtD };

std::string ttml_clock(std::int64_t ms) { return format_clock(ms, ClockStyle{2, '.', 3}); }

Result<Bytes> write_ttml_document(const ir::Subtitles& subs, const WriteContext& ctx, Profile profile) {
  const bool offsets = profile != Profile::EbuTtD && ctx.options.get_enum("ttml_time_format", "clock", {"clock", "offset"}) == "offset";
  util::XmlDocument doc;
  doc.set_declaration("1.0", "UTF-8");
  XmlNode tt = doc.append_child("tt");
  tt.set_attribute("xmlns", ttml_ns);
  tt.set_attribute("xmlns:tts", ttml_styling_ns);
  tt.set_attribute("xmlns:ttp", ttml_parameter_ns);
  tt.set_attribute("xmlns:ttm", ttml_metadata_ns);
  if (profile == Profile::EbuTtD) {
    tt.set_attribute("xmlns:ebuttm", "urn:ebu:tt:metadata");
    tt.set_attribute("xmlns:ebutts", "urn:ebu:tt:style");
  }
  const auto language = subs.metadata.find("language");
  tt.set_attribute("xml:lang", language == subs.metadata.end() ? std::string_view() : std::string_view(language->second));
  tt.set_attribute("ttp:timeBase", "media");
  if (profile == Profile::Imsc1) tt.set_attribute("ttp:profile", "http://www.w3.org/ns/ttml/profile/imsc1/text");
  if (profile != Profile::Ttml) tt.set_attribute("ttp:cellResolution", "50 30");

  XmlNode head = tt.append_child("head");
  const auto title = subs.metadata.find("title");
  if (profile == Profile::EbuTtD) {
    XmlNode meta = head.append_child("metadata").append_child("ebuttm:documentMetadata");
    meta.append_child("ebuttm:documentEbuttVersion").append_text("v1.0");
    if (title != subs.metadata.end()) meta.append_child("ebuttm:documentTitle").append_text(title->second);
  } else if (title != subs.metadata.end()) {
    head.append_child("metadata").append_child("ttm:title").append_text(title->second);
  }
  XmlNode styling = head.append_child("styling");
  XmlNode base = styling.append_child("style");
  base.set_attribute("xml:id", "s0");
  base.set_attribute("tts:fontFamily", profile == Profile::EbuTtD ? "monospaceSansSerif" : "proportionalSansSerif");
  base.set_attribute("tts:fontSize", profile == Profile::Ttml ? "100%" : "1c 1c");
  base.set_attribute("tts:textAlign", "center");
  base.set_attribute("tts:color", "white");
  if (profile == Profile::EbuTtD) base.set_attribute("ebutts:linePadding", "0.5c");
  for (const auto& [id, align] : {std::pair{"italic"sv, "tts:fontStyle"sv}, std::pair{"bold"sv, "tts:fontWeight"sv},
                                  std::pair{"underline"sv, "tts:textDecoration"sv}}) {
    XmlNode style = styling.append_child("style");
    style.set_attribute("xml:id", id);
    style.set_attribute(align, id);
  }
  XmlNode layout = head.append_child("layout");
  for (const auto& [id, display] : {std::pair{"bottom"sv, "after"sv}, std::pair{"middle"sv, "center"sv}, std::pair{"top"sv, "before"sv}}) {
    XmlNode region = layout.append_child("region");
    region.set_attribute("xml:id", id);
    region.set_attribute("tts:origin", "10% 10%");
    region.set_attribute("tts:extent", "80% 80%");
    region.set_attribute("tts:displayAlign", display);
  }

  XmlNode body = tt.append_child("body");
  body.set_attribute("style", "s0");
  XmlNode div = body.append_child("div");
  bool dropped_speakers = false;
  for (std::size_t n = 0; n < subs.cues.size(); ++n) {
    if ((n & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(n) / static_cast<double>(subs.cues.size())));
    const ir::Cue& cue = subs.cues[n];
    XmlNode p = div.append_child("p");
    if (profile == Profile::EbuTtD) p.set_attribute("xml:id", std::format("sub{}", n + 1));
    if (offsets) {
      p.set_attribute("begin", std::format("{}ms", cue.start_ms));
      p.set_attribute("end", std::format("{}ms", cue.end_ms));
    } else {
      p.set_attribute("begin", ttml_clock(cue.start_ms));
      p.set_attribute("end", ttml_clock(cue.end_ms));
    }
    const int alignment = cue.position ? std::clamp(cue.position->alignment, 1, 9) : 2;
    const int row = (alignment - 1) / 3;
    p.set_attribute("region", row == 2 ? "top" : row == 1 ? "middle" : "bottom");
    const int column = (alignment - 1) % 3;
    if (column != 1) p.set_attribute("tts:textAlign", column == 0 ? "left" : "right");
    if (cue.speaker) dropped_speakers = true;
    // One span per run of text with the same formatting.
    bool italic = false, bold = false, underline = false;
    for (const MarkupToken& token : tokenize_markup(cue.text)) {
      switch (token.type) {
        case MarkupToken::Type::Open:
        case MarkupToken::Type::Close: {
          const bool on = token.type == MarkupToken::Type::Open;
          if (token.text == "i") italic = on;
          if (token.text == "b") bold = on;
          if (token.text == "u") underline = on;
          break;
        }
        case MarkupToken::Type::LineBreak: p.append_child("br"); break;
        case MarkupToken::Type::Text: {
          if (!italic && !bold && !underline && profile != Profile::EbuTtD) {
            p.append_text(token.text);
            break;
          }
          XmlNode span = p.append_child("span");
          std::vector<std::string_view> refs;
          if (italic) refs.emplace_back("italic");
          if (bold) refs.emplace_back("bold");
          if (underline) refs.emplace_back("underline");
          if (!refs.empty()) span.set_attribute("style", util::join(refs, " "));
          span.append_text(token.text);
          break;
        }
      }
    }
  }
  if (dropped_speakers) ctx.warn("Speaker names are not written to timed text.");
  util::XmlWriteOptions write;
  write.indent = "  ";
  return to_bytes(util::xml_to_string(doc, write));
}

int sniff_tt(ByteView data, std::string_view target) {
  const std::string head = sniff_prefix(data, 4096);
  if (!head.starts_with('<')) return 0;
  const std::size_t tt = head.find("<tt");
  const std::size_t prefixed = head.find(":tt ");
  if (tt == std::string::npos && prefixed == std::string::npos) return 0;
  const bool ebu = head.find("urn:ebu:tt") != std::string::npos;
  const bool smpte = head.find("smpte-ra.org/schemas/2052") != std::string::npos;
  const bool ttml = head.find("http://www.w3.org/ns/ttml") != std::string::npos ||
                    head.find("http://www.w3.org/2006/10/ttaf1") != std::string::npos ||
                    head.find("http://www.w3.org/2006/04/ttaf1") != std::string::npos;
  if (!ttml && !ebu && !smpte) return 0;
  if (target == "ebu-tt") return ebu ? 95 : 0;
  if (target == "smpte-tt") return smpte && !ebu ? 94 : 0;
  if (target == "dfxp") return 86;
  return 90;
}

// ============================================================================ SAMI

/// Case-insensitive find.
std::size_t ifind(std::string_view text, std::string_view needle, std::size_t from = 0) {
  if (needle.empty() || text.size() < needle.size()) return std::string_view::npos;
  for (std::size_t i = from; i + needle.size() <= text.size(); ++i) {
    if (util::iequals(text.substr(i, needle.size()), needle)) return i;
  }
  return std::string_view::npos;
}

/// Value of an HTML attribute inside a tag ("<SYNC Start=100>").
std::string tag_attribute(std::string_view tag, std::string_view name) {
  std::size_t at = 0;
  while ((at = ifind(tag, name, at)) != std::string_view::npos) {
    const bool boundary = at > 0 && (util::is_ascii_space(tag[at - 1]));
    std::size_t i = at + name.size();
    while (i < tag.size() && util::is_ascii_space(tag[i])) ++i;
    if (!boundary || i >= tag.size() || tag[i] != '=') {
      at += name.size();
      continue;
    }
    ++i;
    while (i < tag.size() && util::is_ascii_space(tag[i])) ++i;
    if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
      const char quote = tag[i];
      const std::size_t close = tag.find(quote, i + 1);
      return std::string(tag.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
    }
    std::size_t end = i;
    while (end < tag.size() && !util::is_ascii_space(tag[end]) && tag[end] != '>') ++end;
    return std::string(tag.substr(i, end - i));
  }
  return {};
}

Result<ir::Subtitles> parse_sami(std::string_view text, const ReadContext& ctx) {
  if (ifind(text.substr(0, std::min<std::size_t>(text.size(), 4096)), "<SAMI") == std::string_view::npos) {
    return fail(Status::Decode, "not a SAMI file (no <SAMI> element)");
  }
  ir::Subtitles subs;
  // Language classes from the style sheet: ".ENUSCC { Name: English; lang: en-US; }".
  std::vector<std::pair<std::string, std::string>> classes;  // class, lang
  if (const std::size_t style = ifind(text, "<STYLE"); style != std::string_view::npos) {
    const std::size_t end = ifind(text, "</STYLE", style);
    const std::string_view css = text.substr(style, end == std::string_view::npos ? std::string_view::npos : end - style);
    std::size_t at = 0;
    while ((at = css.find('.', at)) != std::string_view::npos) {
      std::size_t name_end = at + 1;
      while (name_end < css.size() && (util::is_ascii_alpha(css[name_end]) || util::is_ascii_digit(css[name_end]) || css[name_end] == '-' || css[name_end] == '_')) ++name_end;
      const std::size_t brace = css.find('{', name_end);
      if (name_end == at + 1 || brace == std::string_view::npos || !util::trim(css.substr(name_end, brace - name_end)).empty()) {
        at = name_end;
        continue;
      }
      const std::size_t close = css.find('}', brace);
      const std::string_view body = css.substr(brace + 1, close == std::string_view::npos ? std::string_view::npos : close - brace - 1);
      std::string lang;
      for (const std::string_view declaration : util::split(body, ';', false)) {
        const std::size_t colon = declaration.find(':');
        if (colon != std::string_view::npos && util::iequals(util::trim(declaration.substr(0, colon)), "lang")) {
          lang = std::string(util::trim(declaration.substr(colon + 1)));
        }
      }
      classes.emplace_back(std::string(css.substr(at + 1, name_end - at - 1)), lang);
      at = close == std::string_view::npos ? css.size() : close;
    }
  }
  if (const std::size_t title = ifind(text, "<TITLE>"); title != std::string_view::npos) {
    const std::size_t end = ifind(text, "</TITLE>", title);
    if (end != std::string_view::npos) {
      const std::string value = strip_markup(html_to_ir(text.substr(title + 7, end - title - 7)));
      if (!util::trim(value).empty()) subs.metadata["title"] = std::string(util::trim(value));
    }
  }
  // Which class to read.
  const std::string wanted = ctx.options.get_string("sami_language", "");
  std::string chosen;
  for (const auto& [name, lang] : classes) {
    if (wanted.empty() || util::iequals(name, wanted) || util::iequals(lang, wanted) ||
        (!lang.empty() && util::istarts_with(lang, wanted + "-"))) {
      chosen = name;
      if (!lang.empty()) subs.metadata["language"] = lang;
      break;
    }
  }
  if (!wanted.empty() && chosen.empty() && !classes.empty()) {
    return fail(Status::InvalidArgument, "the SAMI file has no language class '{}'", util::sanitize_utf8(wanted));
  }
  // SYNC blocks.
  struct Sync {
    std::int64_t start;
    std::string text;
  };
  std::vector<Sync> syncs;
  std::size_t at = 0;
  while ((at = ifind(text, "<SYNC", at)) != std::string_view::npos) {
    const std::size_t tag_end = text.find('>', at);
    if (tag_end == std::string_view::npos) break;
    const auto start = util::parse_int(util::trim(tag_attribute(text.substr(at, tag_end - at), "Start")));
    std::size_t next = ifind(text, "<SYNC", tag_end);
    const std::size_t body_end = ifind(text, "</BODY", tag_end);
    if (next == std::string_view::npos || (body_end != std::string_view::npos && body_end < next)) next = body_end;
    const std::string_view content = text.substr(tag_end + 1, next == std::string_view::npos ? std::string_view::npos : next - tag_end - 1);
    at = next == std::string_view::npos ? text.size() : next;
    if (!start || *start < 0) continue;
    // Paragraphs of the chosen class (or all, when the file has no classes).
    std::string paragraph_text;
    bool found = false;
    std::size_t p = 0;
    while ((p = ifind(content, "<P", p)) != std::string_view::npos) {
      if (p + 2 < content.size() && util::is_ascii_alpha(content[p + 2])) {
        p += 2;
        continue;  // <PRE>, <PARAM> ...
      }
      const std::size_t p_end = content.find('>', p);
      if (p_end == std::string_view::npos) break;
      const std::string cls = tag_attribute(content.substr(p, p_end - p), "Class");
      std::size_t stop = ifind(content, "<P", p_end);
      while (stop != std::string_view::npos && stop + 2 < content.size() && util::is_ascii_alpha(content[stop + 2])) {
        stop = ifind(content, "<P", stop + 2);
      }
      if (chosen.empty() || util::iequals(cls, chosen)) {
        paragraph_text.append(content.substr(p_end + 1, stop == std::string_view::npos ? std::string_view::npos : stop - p_end - 1));
        found = true;
      }
      p = stop == std::string_view::npos ? content.size() : stop;
    }
    if (!found && ifind(content, "<P") == std::string_view::npos) {
      paragraph_text = std::string(content);
      found = true;
    }
    if (!found) continue;
    // Line breaks in the source are not breaks in HTML.
    std::string flattened;
    for (const char c : paragraph_text) flattened.push_back(c == '\n' || c == '\r' ? ' ' : c);
    syncs.push_back({*start, html_to_ir(flattened)});
  }
  if (syncs.empty() && ifind(text, "<SYNC") != std::string_view::npos) return fail(Status::Decode, "no readable SAMI <SYNC> blocks");
  std::ranges::stable_sort(syncs, [](const Sync& a, const Sync& b) { return a.start < b.start; });
  for (std::size_t i = 0; i < syncs.size(); ++i) {
    std::string cleaned;
    for (const std::string_view line : util::split(syncs[i].text, '\n')) {
      if (!cleaned.empty()) cleaned.push_back('\n');
      cleaned.append(util::trim(line));
    }
    if (is_blank_text(cleaned)) continue;
    ir::Cue cue;
    cue.start_ms = syncs[i].start;
    cue.end_ms = i + 1 < syncs.size() ? syncs[i + 1].start : syncs[i].start + 5000;
    cue.text = balance_markup(cleaned);
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

Result<std::string> write_sami(const ir::Subtitles& subs, const WriteContext& /*ctx*/) {
  std::string lang = "en-US";
  if (const auto it = subs.metadata.find("language"); it != subs.metadata.end() && !it->second.empty()) lang = it->second;
  std::string cls;
  for (const char c : lang) {
    if (util::is_ascii_alpha(c)) cls.push_back(util::ascii_upper(c));
  }
  cls += "CC";
  std::string title = "Subtitles";
  if (const auto it = subs.metadata.find("title"); it != subs.metadata.end() && !it->second.empty()) title = it->second;
  std::string out = "<SAMI>\n<HEAD>\n<TITLE>" + ir_to_html(title) +
                    "</TITLE>\n<SAMIParam>\n  Metrics {time:ms;}\n  Spec {MSFT:1.0;}\n</SAMIParam>\n"
                    "<STYLE TYPE=\"text/css\">\n<!--\n"
                    "P { font-family: Arial; font-weight: normal; color: white; background-color: black; text-align: center; }\n";
  out += std::format(".{} {{ Name: {}; lang: {}; SAMIType: CC; }}\n-->\n</STYLE>\n</HEAD>\n<BODY>\n", cls, lang, lang);
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    const ir::Cue& cue = subs.cues[i];
    out += std::format("<SYNC Start={}><P Class={}>{}</P></SYNC>\n", cue.start_ms, cls,
                       util::replace_all(ir_to_html(cue.text), "\n", "<br>"));
    if (i + 1 >= subs.cues.size() || subs.cues[i + 1].start_ms > cue.end_ms) {
      out += std::format("<SYNC Start={}><P Class={}>&nbsp;</P></SYNC>\n", cue.end_ms, cls);
    }
  }
  out += "</BODY>\n</SAMI>\n";
  return out;
}

int sniff_sami(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 1024);
  return ifind(head, "<SAMI") != std::string_view::npos && ifind(head, "<SAMI") < 64 ? 95 : 0;
}

// ============================================================================ YouTube srv3

Result<ir::Subtitles> parse_srv3(ByteView input) {
  util::XmlParseOptions options;
  options.preserve_whitespace = true;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  auto parsed = util::parse_xml(input, options);
  if (!parsed) return fail(parsed.error().code, "not a YouTube timed-text file: {}", parsed.error().message);
  const XmlNode root = parsed->root();
  if (root.local_name() != "timedtext") return fail(Status::Decode, "not a YouTube timed-text file (root <{}>)", root.local_name());
  ir::Subtitles subs;
  for (const XmlNode p : root.child_local("body").children_local("p")) {
    const std::int64_t t = p.attribute("t").as_int64(-1);
    const std::int64_t d = p.attribute("d").as_int64(-1);
    if (t < 0 || t > 3'600'000'000LL) continue;
    std::string text;
    for (const XmlNode child : p.children()) {
      if (child.is_text()) text += child.value();
      else if (child.is_element() && child.local_name() == "br") text += "\n";
      else if (child.is_element()) text += child.text();
    }
    ir::Cue cue;
    cue.start_ms = t;
    cue.end_ms = d >= 0 && d < 3'600'000'000LL ? t + d : t + 5000;
    std::string cleaned;
    for (const std::string_view line : util::split(text, '\n')) {
      if (util::trim(line).empty()) continue;
      if (!cleaned.empty()) cleaned.push_back('\n');
      cleaned.append(util::trim(line));
    }
    cue.text = html_to_ir(ir_to_html(cleaned));
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

Result<std::string> write_srv3(const ir::Subtitles& subs, const WriteContext& ctx) {
  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\" ?><timedtext format=\"3\">\n<body>\n";
  bool dropped = false;
  for (const ir::Cue& cue : subs.cues) {
    const std::string plain = strip_markup(cue.text);
    dropped = dropped || plain != cue.text;
    out += std::format("<p t=\"{}\" d=\"{}\">{}</p>\n", cue.start_ms, std::max<std::int64_t>(cue.end_ms - cue.start_ms, 0),
                       util::xml_escape(plain));
  }
  out += "</body>\n</timedtext>\n";
  if (dropped) ctx.warn("Formatting was removed; srv3 output carries plain text.");
  return out;
}

int sniff_srv3(ByteView data, std::string_view /*filename*/) {
  const std::string head = sniff_prefix(data, 1024);
  const std::size_t at = head.find("<timedtext");
  if (at == std::string::npos) return 0;
  return head.find("format=\"3\"", at) != std::string::npos || head.find("format='3'", at) != std::string::npos ? 95 : 70;
}

// ============================================================================ registration

Codec xml_codec(std::string id, std::function<Result<ir::Subtitles>(ByteView, const ReadContext&)> parse,
                std::function<Result<Bytes>(const ir::Subtitles&, const WriteContext&)> write,
                std::vector<OptionSpec> write_options = {}) {
  Codec codec;
  codec.format_id = std::move(id);
  codec.kind = Kind::Subtitles;
  codec.read = [parse = std::move(parse)](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(ir::Subtitles subs, parse(ctx.input, ctx));
    return finish_read(std::move(subs), ctx);
  };
  codec.read_options = common_read_options();
  if (write) {
    codec.write = [write = std::move(write)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      return write(value.get<ir::Subtitles>(), ctx);
    };
    codec.write_options = std::move(write_options);
  }
  return codec;
}

}  // namespace

void register_xml_formats(Registry& registry) {
  const auto ttml_writer = [](Profile fixed) {
    return [fixed](const ir::Subtitles& subs, const WriteContext& ctx) -> Result<Bytes> {
      Profile profile = fixed;
      if (fixed == Profile::Ttml && ctx.options.get_enum("ttml_profile", "ttml", {"ttml", "imsc1"}) == "imsc1") {
        profile = Profile::Imsc1;
      }
      return write_ttml_document(subs, ctx, profile);
    };
  };
  const OptionSpec time_format = OptionSpec::choice("ttml_time_format", "Time format", "clock", {"clock", "offset"},
                                                    "Write times as clock values (00:00:01.500) or offsets (1500ms).")
                                     .advanced();
  const OptionSpec profile = OptionSpec::choice("ttml_profile", "Profile", "ttml", {"ttml", "imsc1"},
                                                "Plain TTML1, or the IMSC1 text profile used by streaming services.");

  for (const auto& [id, name, description, extensions, mime, popularity] :
       {std::tuple{"ttml"sv, "TTML timed text"sv, "W3C Timed Text Markup Language (TTML, IMSC)."sv,
                   std::vector<std::string>{"ttml", "xml"}, "application/ttml+xml"sv, 40},
        std::tuple{"dfxp"sv, "DFXP timed text"sv, "Distribution Format Exchange Profile (TTML1)."sv,
                   std::vector<std::string>{"dfxp", "xml"}, "application/ttaf+xml"sv, 30},
        std::tuple{"ebu-tt"sv, "EBU-TT subtitles"sv, "EBU timed text (Tech 3350 / EBU-TT-D)."sv,
                   std::vector<std::string>{"ebuttd", "xml"}, "application/ttml+xml"sv, 15},
        std::tuple{"smpte-tt"sv, "SMPTE-TT subtitles"sv, "SMPTE ST 2052-1 timed text (text only)."sv,
                   std::vector<std::string>{"smptett", "xml"}, "application/ttml+xml"sv, 10}}) {
    FormatInfo info = subtitle_format(std::string(id), std::string(name), std::string(description), extensions,
                                      std::string(mime), popularity);
    info.sniff = [target = std::string(id)](ByteView data, std::string_view /*filename*/) { return sniff_tt(data, target); };
    registry.add_format(std::move(info));
  }
  registry.add_codec(xml_codec("ttml", parse_ttml_document, ttml_writer(Profile::Ttml), {profile, time_format}));
  registry.add_codec(xml_codec("dfxp", parse_ttml_document, ttml_writer(Profile::Ttml), {time_format}));
  registry.add_codec(xml_codec("ebu-tt", parse_ttml_document, ttml_writer(Profile::EbuTtD)));
  registry.add_codec(xml_codec("smpte-tt", parse_ttml_document, nullptr));

  FormatInfo sami = subtitle_format("sami", "SAMI captions", "Microsoft Synchronized Accessible Media Interchange.",
                                    {"smi", "sami"}, "application/x-sami", 25);
  sami.sniff = sniff_sami;
  registry.add_format(std::move(sami));
  registry.add_codec(make_text_codec(
      "sami", parse_sami, write_sami,
      {OptionSpec::text("sami_language", "Language", "",
                        "Language class to read from a multi-language SAMI file (class name such as ENUSCC or a "
                        "language tag such as en-US); empty = the first one.")},
      {}, "crlf"));

  FormatInfo srv3 = subtitle_format("srv3", "YouTube timed text (srv3)", "YouTube's XML caption format 3.",
                                    {"srv3", "ytt"}, "application/xml", 10);
  srv3.sniff = sniff_srv3;
  registry.add_format(std::move(srv3));
  registry.add_codec(xml_codec(
      "srv3", [](ByteView input, const ReadContext&) { return parse_srv3(input); },
      [](const ir::Subtitles& subs, const WriteContext& ctx) -> Result<Bytes> {
        CVT_TRY_ASSIGN(std::string text, write_srv3(subs, ctx));
        return to_bytes(text);
      }));
}

}  // namespace cvt::subtitle
