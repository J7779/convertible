// Convertible core - subtitle family: Advanced SubStation Alpha (.ass) and
// SubStation Alpha (.ssa).
//
// Reading keeps the script header (PlayRes, WrapStyle, ...) and every style
// in the track metadata ("ass.<key>", "ass.style.<name>" holding the 23 V4+
// style fields), maps override tags to IR markup (\i \b \u, \r), positions
// (\an, legacy \a, \pos, \move) and keeps the original text of cues whose
// tags cannot be mapped (colours, karaoke, transforms, drawings) in
// Cue::source_text so that ass -> ass round trips lose nothing. Styles with
// italic / bold / underline turned on make the IR text italic / bold /
// underlined, so that other formats show what the script shows.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
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

constexpr std::array<std::string_view, 23> v4plus_fields = {
    "name",      "fontname",  "fontsize", "primarycolour", "secondarycolour", "outlinecolour", "backcolour", "bold",
    "italic",    "underline", "strikeout", "scalex",       "scaley",          "spacing",       "angle",      "borderstyle",
    "outline",   "shadow",    "alignment", "marginl",      "marginr",         "marginv",       "encoding"};
constexpr std::array<std::string_view, 23> v4plus_names = {
    "Name",      "Fontname",  "Fontsize",  "PrimaryColour", "SecondaryColour", "OutlineColour", "BackColour", "Bold",
    "Italic",    "Underline", "StrikeOut", "ScaleX",        "ScaleY",          "Spacing",       "Angle",      "BorderStyle",
    "Outline",   "Shadow",    "Alignment", "MarginL",       "MarginR",         "MarginV",       "Encoding"};
constexpr std::array<std::string_view, 23> v4plus_defaults = {
    "Default", "Arial", "20", "&H00FFFFFF", "&H000000FF", "&H00000000", "&H00000000", "0", "0", "0", "0", "100",
    "100",     "0",     "0",  "1",          "2",          "2",          "2",          "10", "10", "10", "1"};
constexpr std::array<std::string_view, 18> v4_names = {
    "Name",        "Fontname", "Fontsize", "PrimaryColour", "SecondaryColour", "TertiaryColour",
    "BackColour",  "Bold",     "Italic",   "BorderStyle",   "Outline",         "Shadow",
    "Alignment",   "MarginL",  "MarginR",  "MarginV",       "AlphaLevel",      "Encoding"};

std::size_t field_index(std::string_view name) {
  for (std::size_t i = 0; i < v4plus_fields.size(); ++i) {
    if (v4plus_fields[i] == name) return i;
  }
  return v4plus_fields.size();
}

/// Legacy SSA alignment (1-3 bottom, 5-7 top, 9-11 middle) to keypad layout.
int legacy_to_numpad(int a) {
  if (a >= 1 && a <= 3) return a;
  if (a >= 5 && a <= 7) return a + 2;
  if (a >= 9 && a <= 11) return a - 5;
  return 2;
}
int numpad_to_legacy(int a) {
  if (a >= 1 && a <= 3) return a;
  if (a >= 7 && a <= 9) return a - 2;
  if (a >= 4 && a <= 6) return a + 5;
  return 2;
}

bool flag_on(std::string_view value) {
  const auto number = util::parse_int(util::trim(value));
  return number && *number != 0;
}

std::string lower_key(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c != ' ') out.push_back(util::ascii_lower(c));
  }
  return out;
}

/// "a, b, c" split into exactly `count` fields, the last taking the rest.
std::vector<std::string_view> split_fields(std::string_view text, std::size_t count) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (out.size() + 1 < count) {
    const std::size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) break;
    out.push_back(util::trim(text.substr(start, comma - start)));
    start = comma + 1;
  }
  out.push_back(text.substr(std::min(start, text.size())));
  return out;
}

struct Script {
  double play_res_x = 0.0;
  double play_res_y = 0.0;
  bool wrap_style_2 = false;
  std::map<std::string, std::vector<std::string>> styles;  // canonical V4+ fields
};

struct Converted {
  std::string text;
  bool unmapped = false;
};

/// ASS dialogue text to IR markup and placement.
Converted ass_to_ir(std::string_view text, ir::Cue& cue, const Script& script) {
  Converted out;
  std::string result;
  std::vector<char> open;  // currently open IR tags
  bool drawing = false;
  const auto set = [&](char tag, bool on) {
    const auto it = std::ranges::find(open, tag);
    if (on && it == open.end()) {
      open.push_back(tag);
      result += std::format("<{}>", tag);
    } else if (!on && it != open.end()) {
      open.erase(it);
      result += std::format("</{}>", tag);
    }
  };
  const auto place = [&](int alignment) {
    ir::CuePosition position = cue.position.value_or(ir::CuePosition{});
    position.alignment = alignment;
    cue.position = position;
  };
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '{') {
      const std::size_t close = text.find('}', i);
      if (close == std::string_view::npos) {
        result.push_back(c);
        ++i;
        continue;
      }
      const std::string_view block = text.substr(i + 1, close - i - 1);
      if (block.find('\\') == std::string_view::npos) out.unmapped = out.unmapped || !block.empty();  // a comment
      for (std::string_view tag : util::split(block, '\\', false)) {
        tag = util::trim(tag);
        if (tag.empty()) continue;
        const auto number_after = [&](std::size_t prefix) { return util::parse_int(util::trim(tag.substr(prefix))); };
        if (tag == "i" || tag == "i0" || tag == "i1") {
          set('i', tag == "i1");
        } else if (tag == "u" || tag == "u0" || tag == "u1") {
          set('u', tag == "u1");
        } else if (tag.size() >= 1 && tag[0] == 'b' && (tag.size() == 1 || util::is_ascii_digit(tag[1]))) {
          const auto weight = tag.size() == 1 ? std::optional<std::int64_t>(0) : number_after(1);
          set('b', weight && (*weight == 1 || *weight >= 600));
        } else if (tag.starts_with("an") && tag.size() == 3 && tag[2] >= '1' && tag[2] <= '9') {
          place(tag[2] - '0');
        } else if (tag[0] == 'a' && tag.size() >= 2 && util::is_ascii_digit(tag[1])) {
          if (const auto legacy = number_after(1)) place(legacy_to_numpad(static_cast<int>(*legacy)));
        } else if (tag.starts_with("pos(") || tag.starts_with("move(")) {
          const std::size_t open_paren = tag.find('(');
          const std::size_t close_paren = tag.find(')');
          const std::vector<std::string_view> args =
              util::split(tag.substr(open_paren + 1, (close_paren == std::string_view::npos ? tag.size() : close_paren) - open_paren - 1), ',');
          if (args.size() >= 2 && script.play_res_x > 0 && script.play_res_y > 0) {
            const auto x = util::parse_double(util::trim(args[0]));
            const auto y = util::parse_double(util::trim(args[1]));
            if (x && y && std::isfinite(*x) && std::isfinite(*y)) {
              ir::CuePosition position = cue.position.value_or(ir::CuePosition{});
              position.x = std::clamp(*x / script.play_res_x, 0.0, 1.0);
              position.y = std::clamp(*y / script.play_res_y, 0.0, 1.0);
              cue.position = position;
            }
          }
          if (tag.starts_with("move(")) out.unmapped = true;
        } else if (tag[0] == 'r') {
          for (auto it = open.rbegin(); it != open.rend(); ++it) result += std::format("</{}>", *it);
          open.clear();
          if (tag.size() > 1) out.unmapped = true;
        } else if (tag[0] == 'p' && tag.size() >= 2 && util::is_ascii_digit(tag[1])) {
          drawing = number_after(1).value_or(0) > 0;
          out.unmapped = true;
        } else {
          out.unmapped = true;  // colours, fonts, karaoke, transforms, clips, ...
        }
      }
      i = close + 1;
      continue;
    }
    if (c == '\\' && i + 1 < text.size()) {
      const char next = text[i + 1];
      if (next == 'N') {
        result.push_back('\n');
        i += 2;
        continue;
      }
      if (next == 'n') {
        result.push_back(script.wrap_style_2 ? '\n' : ' ');
        i += 2;
        continue;
      }
      if (next == 'h') {
        result += "\xC2\xA0";
        i += 2;
        continue;
      }
    }
    if (!drawing) result.push_back(c);
    ++i;
  }
  for (auto it = open.rbegin(); it != open.rend(); ++it) result += std::format("</{}>", *it);
  out.text = balance_markup(result);
  return out;
}

/// The style's own italic/bold/underline, applied to the whole cue.
std::string apply_style_flags(std::string text, const std::vector<std::string>* style) {
  if (style == nullptr) return text;
  std::string open;
  std::string close;
  if (flag_on((*style)[field_index("underline")])) open += "<u>", close.insert(0, "</u>");
  if (flag_on((*style)[field_index("bold")])) open += "<b>", close.insert(0, "</b>");
  if (flag_on((*style)[field_index("italic")])) open += "<i>", close.insert(0, "</i>");
  if (open.empty()) return text;
  // Overrides inside may switch the flag off; balance_markup keeps the result well-formed.
  return balance_markup(open + text + close);
}

std::vector<std::string> canonical_style(const std::vector<std::string_view>& format, std::string_view values,
                                         bool legacy) {
  std::vector<std::string> style(v4plus_defaults.begin(), v4plus_defaults.end());
  const std::vector<std::string_view> fields = split_fields(values, format.size());
  for (std::size_t f = 0; f < format.size() && f < fields.size(); ++f) {
    std::string key = lower_key(format[f]);
    if (key == "tertiarycolour") key = "outlinecolour";
    const std::size_t index = field_index(key);
    if (index >= style.size()) continue;
    std::string value(util::trim(fields[f]));
    if (key == "alignment" && legacy) {
      value = std::to_string(legacy_to_numpad(static_cast<int>(util::parse_int(value).value_or(2))));
    }
    style[index] = value;
  }
  if (style[0].empty()) style[0] = "Default";
  return style;
}

Result<ir::Subtitles> parse_ass(std::string_view text, const ReadContext& ctx) {
  ir::Subtitles subs;
  Script script;
  std::string section;
  std::vector<std::string_view> style_format;
  std::vector<std::string_view> event_format;
  bool legacy_styles = false;
  bool saw_section = false;
  bool skipped_fonts = false;
  for (const std::string_view raw : text_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.empty() || line.starts_with(';') || line.starts_with("!:")) continue;
    if (line.starts_with('[') && line.ends_with(']')) {
      section = util::to_lower(line.substr(1, line.size() - 2));
      saw_section = true;
      if (section == "v4 styles") legacy_styles = true;
      if (section == "fonts" || section == "graphics") skipped_fonts = true;
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view key = util::trim(line.substr(0, colon));
    const std::string_view value = util::trim(line.substr(colon + 1));
    if (section == "script info") {
      if (util::iequals(key, "Title")) {
        if (!value.empty() && value != "<untitled>") subs.metadata["title"] = std::string(value);
      } else {
        subs.metadata["ass." + std::string(key)] = std::string(value);
      }
      if (util::iequals(key, "PlayResX")) script.play_res_x = util::parse_double(value).value_or(0.0);
      if (util::iequals(key, "PlayResY")) script.play_res_y = util::parse_double(value).value_or(0.0);
      if (util::iequals(key, "WrapStyle")) script.wrap_style_2 = value == "2";
    } else if (section == "v4+ styles" || section == "v4 styles") {
      if (util::iequals(key, "Format")) {
        style_format = util::split(value, ',');
        for (std::string_view& name : style_format) name = util::trim(name);
      } else if (util::iequals(key, "Style")) {
        const std::vector<std::string_view> format =
            style_format.empty() ? std::vector<std::string_view>(legacy_styles ? v4_names.begin() : v4plus_names.begin(),
                                                                 legacy_styles ? v4_names.end() : v4plus_names.end())
                                 : style_format;
        std::vector<std::string> style = canonical_style(format, value, legacy_styles);
        subs.metadata["ass.style." + style[0]] = util::join(style, ",");
        script.styles[style[0]] = std::move(style);
      }
    } else if (section == "events") {
      if (util::iequals(key, "Format")) {
        event_format = util::split(value, ',');
        for (std::string_view& name : event_format) name = util::trim(name);
        continue;
      }
      if (!util::iequals(key, "Dialogue")) continue;  // Comment, Picture, Sound, Movie, Command
      static constexpr std::array<std::string_view, 10> default_events = {
          "Layer", "Start", "End", "Style", "Name", "MarginL", "MarginR", "MarginV", "Effect", "Text"};
      const std::vector<std::string_view> format =
          event_format.empty() ? std::vector<std::string_view>(default_events.begin(), default_events.end()) : event_format;
      const std::vector<std::string_view> fields = split_fields(line.substr(colon + 1), format.size());
      if (fields.size() != format.size()) continue;
      ir::Cue cue;
      std::optional<std::int64_t> start;
      std::optional<std::int64_t> end;
      std::string_view body;
      for (std::size_t f = 0; f < format.size(); ++f) {
        const std::string field = lower_key(format[f]);
        const std::string_view v = f + 1 == format.size() ? fields[f] : util::trim(fields[f]);
        if (field == "start") start = parse_clock(v);
        else if (field == "end") end = parse_clock(v);
        else if (field == "style") cue.style = std::string(v.starts_with('*') ? v.substr(1) : v);
        else if (field == "name" || field == "actor") {
          if (!v.empty()) cue.speaker = std::string(v);
        } else if (field == "text") body = v;
        else if (field == "layer" && v != "0") cue.attributes["ass.layer"] = std::string(v);
        else if (field == "effect" && !v.empty()) cue.attributes["ass.effect"] = std::string(v);
        else if ((field == "marginl" || field == "marginr" || field == "marginv") && !v.empty() &&
                 util::parse_int(v).value_or(0) != 0) {
          cue.attributes["ass." + field] = std::string(v);
        }
      }
      if (!start || !end) continue;
      cue.start_ms = *start;
      cue.end_ms = *end;
      const auto style = cue.style ? script.styles.find(*cue.style) : script.styles.end();
      const std::vector<std::string>* style_fields = style == script.styles.end() ? nullptr : &style->second;
      if (style_fields != nullptr && !cue.position) {
        const int alignment = static_cast<int>(util::parse_int((*style_fields)[field_index("alignment")]).value_or(2));
        if (alignment != 2 && alignment >= 1 && alignment <= 9) {
          ir::CuePosition position;
          position.alignment = alignment;
          cue.position = position;
        }
      }
      Converted converted = ass_to_ir(body, cue, script);
      cue.text = apply_style_flags(std::move(converted.text), style_fields);
      if (converted.unmapped) {
        cue.source_text = std::string(body);
        cue.source_format = ctx.format_id;
      }
      subs.cues.push_back(std::move(cue));
    }
  }
  if (!saw_section && !util::trim(text).empty()) return fail(Status::Decode, "not a SubStation Alpha script (no [Script Info] or [Events] section)");
  if (skipped_fonts) ctx.warn("Embedded fonts and graphics of the script were not kept.");
  return subs;
}

// ---------------------------------------------------------------------------- writing

std::string ir_to_ass(std::string_view ir_text) {
  std::string out;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    switch (token.type) {
      case MarkupToken::Type::Text: out += token.text; break;
      case MarkupToken::Type::LineBreak: out += "\\N"; break;
      case MarkupToken::Type::Open: out += "{\\" + token.text + "1}"; break;
      case MarkupToken::Type::Close: out += "{\\" + token.text + "0}"; break;
    }
  }
  // Merge adjacent override blocks: "{\i1}{\b1}" -> "{\i1\b1}".
  return util::replace_all(out, "}{\\", "\\");
}

Result<std::string> write_ass_common(const ir::Subtitles& subs, const WriteContext& ctx, bool ssa) {
  const auto meta = [&](std::string_view key) -> std::string {
    const auto it = subs.metadata.find(std::string(key));
    return it == subs.metadata.end() ? std::string() : it->second;
  };
  double play_x = util::parse_double(meta("ass.PlayResX")).value_or(0.0);
  double play_y = util::parse_double(meta("ass.PlayResY")).value_or(0.0);
  if (!(play_x > 0 && play_x < 100000) || !(play_y > 0 && play_y < 100000)) {
    play_x = 384;
    play_y = 288;
  }
  std::string out = "[Script Info]\n";
  const std::string title = meta("title");
  out += "Title: " + (title.empty() ? std::string("<untitled>") : util::replace_all(title, "\n", " ")) + "\n";
  out += ssa ? "ScriptType: v4.00\n" : "ScriptType: v4.00+\n";
  out += std::format("PlayResX: {}\nPlayResY: {}\n", util::format_double(play_x), util::format_double(play_y));
  for (const auto& [key, value] : subs.metadata) {
    if (!key.starts_with("ass.") || key.starts_with("ass.style.")) continue;
    const std::string_view name = std::string_view(key).substr(4);
    if (name == "PlayResX" || name == "PlayResY" || name == "ScriptType" || name.find('\n') != std::string_view::npos ||
        value.find('\n') != std::string::npos) {
      continue;
    }
    out += std::format("{}: {}\n", name, value);
  }
  if (!subs.metadata.contains("ass.WrapStyle")) out += "WrapStyle: 0\n";
  if (!ssa && !subs.metadata.contains("ass.ScaledBorderAndShadow")) out += "ScaledBorderAndShadow: yes\n";
  out += "\n";

  // Styles: those of the source plus a default for every style the cues use.
  std::map<std::string, std::vector<std::string>> styles;
  for (const auto& [key, value] : subs.metadata) {
    if (!key.starts_with("ass.style.")) continue;
    std::vector<std::string> fields;
    for (const std::string_view f : util::split(value, ',')) fields.emplace_back(f);
    if (fields.size() != v4plus_fields.size()) continue;
    styles[fields[0]] = std::move(fields);
  }
  const auto ensure_style = [&](const std::string& name) {
    if (styles.contains(name)) return;
    std::vector<std::string> fields(v4plus_defaults.begin(), v4plus_defaults.end());
    fields[0] = name;
    styles[name] = std::move(fields);
  };
  ensure_style("Default");
  for (const ir::Cue& cue : subs.cues) {
    if (cue.style && !cue.style->empty() && cue.style->find(',') == std::string::npos) ensure_style(*cue.style);
  }
  if (ssa) {
    out += "[V4 Styles]\nFormat: " + util::join(std::vector<std::string_view>(v4_names.begin(), v4_names.end()), ", ") + "\n";
    for (const auto& [name, f] : styles) {
      const auto get = [&](std::string_view field) { return f[field_index(field)]; };
      const int alignment = numpad_to_legacy(static_cast<int>(util::parse_int(get("alignment")).value_or(2)));
      out += std::format("Style: {},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},0,{}\n", get("name"), get("fontname"),
                         get("fontsize"), get("primarycolour"), get("secondarycolour"), get("outlinecolour"),
                         get("backcolour"), get("bold"), get("italic"), get("borderstyle"), get("outline"),
                         get("shadow"), alignment, get("marginl"), get("marginr"), get("marginv"), get("encoding"));
    }
    out += "\n[Events]\nFormat: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
  } else {
    out += "[V4+ Styles]\nFormat: " + util::join(std::vector<std::string_view>(v4plus_names.begin(), v4plus_names.end()), ", ") + "\n";
    for (const auto& [name, f] : styles) out += "Style: " + util::join(f, ",") + "\n";
    out += "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
  }

  Script script;
  script.play_res_x = play_x;
  script.play_res_y = play_y;
  script.wrap_style_2 = meta("ass.WrapStyle") == "2";
  script.styles = styles;
  const ClockStyle clock{1, '.', 2};
  for (std::size_t n = 0; n < subs.cues.size(); ++n) {
    if ((n & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(n) / static_cast<double>(subs.cues.size())));
    const ir::Cue& cue = subs.cues[n];
    const std::string style_name =
        cue.style && !cue.style->empty() && cue.style->find(',') == std::string::npos ? *cue.style : std::string("Default");
    const std::vector<std::string>* style = &styles[style_name];
    const auto attribute = [&](std::string_view key, std::string_view fallback) {
      const auto it = cue.attributes.find(std::string(key));
      return it == cue.attributes.end() ? std::string(fallback) : util::replace_all(it->second, "\n", " ");
    };
    std::string body;
    bool used_source = false;
    if (!cue.source_text.empty() && (cue.source_format == "ass" || cue.source_format == "ssa")) {
      // Use the original markup when it still renders to this cue's text.
      ir::Cue probe;
      const Converted converted = ass_to_ir(cue.source_text, probe, script);
      if (apply_style_flags(converted.text, style) == cue.text) {
        body = cue.source_text;
        used_source = true;
      }
    }
    if (!used_source) {
      std::string prefix;
      const int style_alignment = static_cast<int>(util::parse_int((*style)[field_index("alignment")]).value_or(2));
      if (cue.position) {
        if (cue.position->alignment != style_alignment && cue.position->alignment >= 1 && cue.position->alignment <= 9) {
          prefix += ssa ? std::format("\\a{}", numpad_to_legacy(cue.position->alignment))
                        : std::format("\\an{}", cue.position->alignment);
        }
        if (cue.position->x && cue.position->y) {
          prefix += std::format("\\pos({},{})", std::llround(*cue.position->x * play_x), std::llround(*cue.position->y * play_y));
        }
      }
      std::string text = cue.text;
      // Tags the style already applies are not repeated.
      for (const auto& [field, tag] : {std::pair{"italic"sv, "i"sv}, std::pair{"bold"sv, "b"sv}, std::pair{"underline"sv, "u"sv}}) {
        if (!flag_on((*style)[field_index(field)])) continue;
        const std::string open = std::format("<{}>", tag);
        const std::string close = std::format("</{}>", tag);
        if (text.starts_with(open) && text.ends_with(close) && text.find(close) == text.size() - close.size()) {
          text = text.substr(open.size(), text.size() - open.size() - close.size());
        } else {
          prefix += std::format("\\{}0", tag);
        }
      }
      body = (prefix.empty() ? std::string() : "{" + prefix + "}") + ir_to_ass(text);
    }
    body = util::replace_all(util::replace_all(body, "\r", ""), "\n", "\\N");
    const std::string name = cue.speaker ? util::replace_all(util::replace_all(*cue.speaker, ",", ";"), "\n", " ") : std::string();
    const std::string effect = util::replace_all(attribute("ass.effect", ""), ",", ";");
    if (ssa) {
      out += std::format("Dialogue: Marked=0,{},{},{},{},{},{},{},{},{}\n", format_clock(cue.start_ms, clock),
                         format_clock(cue.end_ms, clock), style_name, name, attribute("ass.marginl", "0"),
                         attribute("ass.marginr", "0"), attribute("ass.marginv", "0"), effect, body);
    } else {
      out += std::format("Dialogue: {},{},{},{},{},{},{},{},{},{}\n", attribute("ass.layer", "0"),
                         format_clock(cue.start_ms, clock), format_clock(cue.end_ms, clock), style_name, name,
                         attribute("ass.marginl", "0"), attribute("ass.marginr", "0"), attribute("ass.marginv", "0"),
                         effect, body);
    }
  }
  return out;
}

int sniff_ass(ByteView data, std::string_view /*filename*/, bool want_ssa) {
  const std::string head = sniff_prefix(data, 4096);
  if (!util::istarts_with(head, "[Script Info]")) return 0;
  const std::size_t type = head.find("ScriptType:");
  bool is_plus = head.find("[V4+ Styles]") != std::string::npos;
  if (type != std::string::npos) {
    const std::string_view value = util::trim(std::string_view(head).substr(type + 11, 12));
    is_plus = util::istarts_with(value, "v4.00+") || util::istarts_with(value, "v4.00 +");
  }
  if (head.find("[V4 Styles]") != std::string::npos) is_plus = false;
  return is_plus == !want_ssa ? 95 : 60;
}

}  // namespace

void register_ass(Registry& registry) {
  FormatInfo ass = subtitle_format("ass", "Advanced SubStation Alpha subtitles",
                                   "Styled subtitles with positioning and effects (Aegisub, libass).", {"ass"},
                                   "text/x-ssa", 55);
  ass.sniff = [](ByteView data, std::string_view filename) { return sniff_ass(data, filename, false); };
  registry.add_format(std::move(ass));
  registry.add_codec(make_text_codec(
      "ass", parse_ass, [](const ir::Subtitles& subs, const WriteContext& ctx) { return write_ass_common(subs, ctx, false); }));

  FormatInfo ssa = subtitle_format("ssa", "SubStation Alpha subtitles", "The original SubStation Alpha v4 script format.",
                                   {"ssa"}, "text/x-ssa", 35);
  ssa.sniff = [](ByteView data, std::string_view filename) { return sniff_ass(data, filename, true); };
  registry.add_format(std::move(ssa));
  registry.add_codec(make_text_codec(
      "ssa", parse_ass, [](const ir::Subtitles& subs, const WriteContext& ctx) { return write_ass_common(subs, ctx, true); }));
}

}  // namespace cvt::subtitle
