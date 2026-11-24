// Convertible core - code family: syntax-highlighted output.
//
//   html-highlight  standalone HTML page (or fragment) with light and dark
//                   colour themes and optional line numbers
//   rtf-highlight   RTF with a colour table, for word processors and slides
//   ansi            text with ANSI/VT100 colour escapes for terminals
//
// All three write a Document: code blocks are highlighted with the token
// stream of highlight.hpp (language from the block, the `language` option, or
// the source format), other blocks are written as plain paragraphs.
//
// The `ansi` format is also readable: escape sequences are removed and the
// text becomes a code block, so coloured logs convert to HTML, PDF or text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "families/code/highlight.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

using TC = TokenClass;

constexpr std::size_t index_of(TC cls) { return static_cast<std::size_t>(cls); }

// Colours per token class: light theme, dark theme (both "rrggbb"), ANSI SGR.
struct Style {
  std::string_view light;
  std::string_view dark;
  std::string_view sgr16;
  int xterm256;
  bool bold;
  bool italic;
};
constexpr std::array<Style, token_class_count> styles = {{
    {"24292f", "e6edf3", "", -1, false, false},      // plain
    {"cf222e", "ff7b72", "1;31", 203, true, false},  // keyword
    {"953800", "ffa657", "33", 215, false, false},   // type
    {"0a3069", "a5d6ff", "32", 114, false, false},   // string
    {"0550ae", "79c0ff", "36", 117, false, false},   // number
    {"6e7781", "8b949e", "2;37", 245, false, true},  // comment
    {"24292f", "e6edf3", "", -1, false, false},      // operator
    {"8250df", "d2a8ff", "35", 177, false, false},   // preprocessor
    {"6639ba", "d2a8ff", "1;35", 141, false, false}, // function
    {"0550ae", "79c0ff", "1;36", 75, false, false},  // constant
    {"953800", "ffa657", "33", 179, false, false},   // variable
    {"116329", "7ee787", "1;32", 78, false, false},  // tag
    {"0550ae", "79c0ff", "36", 117, false, false},   // attribute
    {"116329", "aff5b4", "32", 71, false, false},    // inserted
    {"82071e", "ffdcd7", "31", 167, false, false},   // deleted
    {"0550ae", "79c0ff", "1;34", 69, true, false},   // heading
}};

std::string block_text(const ir::Block& block) {
  std::string out;
  ir::detail::append_plain_block(out, block, {});
  return out;
}

int hex_component(std::string_view rgb, std::size_t at) {
  return static_cast<int>(util::parse_int(rgb.substr(at, 2), 16).value_or(0));
}

std::string_view block_language(const ir::CodeBlock& block, const WriteContext& ctx, std::string& storage) {
  storage = ctx.options.get_string("language", "auto");
  if (!storage.empty() && storage != "auto") return storage;
  if (!block.language.empty()) return block.language;
  return ctx.source_format_id;
}

/// A document flattened into a list of (language, text) segments; text
/// blocks get an empty language and are written unhighlighted.
struct Segment {
  std::string language;
  std::string text;
  bool code = false;
  int heading = 0;  // heading level, 0 = not a heading
};

std::vector<Segment> segments_of(const ir::Document& document, const WriteContext& ctx) {
  std::vector<Segment> out;
  std::string storage;
  for (const ir::Block& block : document.blocks) {
    if (const auto* code = block.get_if<ir::CodeBlock>()) {
      out.push_back(Segment{std::string(block_language(*code, ctx, storage)), code->code, true, 0});
    } else if (const auto* heading = block.get_if<ir::Heading>()) {
      out.push_back(Segment{{}, ir::plain_text(heading->content), false, heading->level});
    } else {
      std::string text = block_text(block);
      if (!text.empty() && text.back() == '\n') text.pop_back();
      out.push_back(Segment{{}, std::move(text), false, 0});
    }
  }
  return out;
}

// ---------------------------------------------------------------------------- HTML

void html_escape(std::string& out, std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
}

std::string html_css(std::string_view theme) {
  std::string light, dark;
  for (std::size_t i = 1; i < token_class_count; ++i) {
    const Style& s = styles[i];
    const std::string_view name = token_class_name(static_cast<TC>(i));
    std::string extra;
    if (s.bold) extra += "font-weight:600;";
    if (s.italic) extra += "font-style:italic;";
    light += std::format(".hl-{}{{color:#{};{}}}\n", name, s.light, extra);
    dark += std::format(".hl-{}{{color:#{}}}\n", name, s.dark);
  }
  light += ".hl-inserted{background:#dafbe1}\n.hl-deleted{background:#ffebe9}\n";
  dark += ".hl-inserted{background:#033a16}\n.hl-deleted{background:#67060c}\n";
  std::string css =
      ":root{color-scheme:light dark}\n"
      "body{margin:0;padding:1.5rem;background:#ffffff;color:#24292f;"
      "font-family:system-ui,-apple-system,'Segoe UI',sans-serif}\n"
      "pre.hl{margin:0 0 1rem;padding:1rem;overflow:auto;border-radius:6px;background:#f6f8fa;color:#24292f;"
      "font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;tab-size:var(--tab,4)}\n"
      "pre.hl .ln{display:block;counter-increment:line}\n"
      "pre.hl .ln::before{content:counter(line);display:inline-block;width:3em;margin-right:1em;padding-right:.5em;"
      "text-align:right;color:#8c959f;border-right:1px solid #d0d7de;user-select:none}\n"
      "pre.hl.numbered{counter-reset:line}\n";
  const std::string dark_base =
      "body{background:#0d1117;color:#e6edf3}\npre.hl{background:#161b22;color:#e6edf3}\n"
      "pre.hl .ln::before{color:#6e7681;border-color:#30363d}\n";
  if (theme == "dark") return css + light + dark_base + dark;
  css += light;
  if (theme == "auto") css += "@media (prefers-color-scheme: dark){\n" + dark_base + dark + "}\n";
  return css;
}

void html_code(std::string& out, std::string_view code, std::string_view language, bool numbers) {
  const std::vector<Token> tokens = tokenize(code, language);
  out += numbers ? "<pre class=\"hl numbered\"><code" : "<pre class=\"hl\"><code";
  if (!language.empty()) {
    out += " class=\"language-";
    html_escape(out, language);
    out += '"';
  }
  out += '>';
  bool line_open = false;
  const auto open_line = [&] {
    if (numbers && !line_open) {
      out += "<span class=\"ln\">";
      line_open = true;
    }
  };
  for (const Token& token : tokens) {
    std::string_view text = code.substr(token.offset, token.length);
    // Spans never cross line boundaries, so numbered lines stay well-formed.
    while (!text.empty()) {
      open_line();
      const std::size_t nl = text.find('\n');
      const std::string_view piece = text.substr(0, nl);
      if (!piece.empty()) {
        if (token.cls == TC::Plain || token.cls == TC::Operator) {  // operators keep the text colour
          html_escape(out, piece);
        } else {
          out += std::format("<span class=\"hl-{}\">", token_class_name(token.cls));
          html_escape(out, piece);
          out += "</span>";
        }
      }
      if (nl == std::string_view::npos) break;
      if (numbers) {
        out += "</span>";
        line_open = false;
      }
      out += '\n';
      text.remove_prefix(nl + 1);
    }
  }
  if (line_open) out += "</span>";
  out += "</code></pre>\n";
}

Result<Bytes> write_html(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& document = value.get<ir::Document>();
  const bool numbers = ctx.options.get_bool("line_numbers", false);
  const std::string theme = ctx.options.get_enum("theme", "auto", {"auto", "light", "dark"});
  const bool standalone = ctx.options.get_bool("standalone", true);
  const auto tab = ctx.options.get_int("tab_size", 4, 1, 16);
  std::string body;
  for (const Segment& segment : segments_of(document, ctx)) {
    CVT_TRY(ctx.checkpoint(0.5));
    if (segment.code) {
      html_code(body, segment.text, segment.language, numbers);
    } else if (segment.heading > 0) {
      const int level = std::clamp(segment.heading, 1, 6);
      body += std::format("<h{}>", level);
      html_escape(body, segment.text);
      body += std::format("</h{}>\n", level);
    } else if (!segment.text.empty()) {
      body += "<p>";
      html_escape(body, segment.text);
      body += "</p>\n";
    }
  }
  if (!standalone) return to_bytes(body);
  std::string title = document.metadata.title.empty() ? ctx.source_filename : document.metadata.title;
  if (title.empty()) title = "Source code";
  std::string out = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>";
  html_escape(out, util::sanitize_utf8(title));
  out += "</title>\n<style>\n";
  out += html_css(theme);
  out += std::format("pre.hl{{--tab:{}}}\n", tab);
  out += "</style>\n</head>\n<body>\n";
  out += body;
  out += "</body>\n</html>\n";
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

// ---------------------------------------------------------------------------- RTF

void rtf_escape(std::string& out, std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const char32_t cp = d.code_point;
    i += d.length;
    if (cp == '\\' || cp == '{' || cp == '}') {
      out.push_back('\\');
      out.push_back(static_cast<char>(cp));
    } else if (cp == '\t') {
      out += "\\tab ";
    } else if (cp == '\r') {
    } else if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x10000) {
      out += std::format("\\u{}?", static_cast<std::int16_t>(static_cast<std::uint16_t>(cp)));
    } else {  // a surrogate pair
      const std::uint32_t v = static_cast<std::uint32_t>(cp) - 0x10000u;
      out += std::format("\\u{}?\\u{}?", static_cast<std::int16_t>(static_cast<std::uint16_t>(0xD800u + (v >> 10))),
                         static_cast<std::int16_t>(static_cast<std::uint16_t>(0xDC00u + (v & 0x3FFu))));
    }
  }
}

Result<Bytes> write_rtf(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& document = value.get<ir::Document>();
  const auto half_points = ctx.options.get_int("font_size", 10, 6, 72) * 2;
  const bool numbers = ctx.options.get_bool("line_numbers", false);
  std::string out = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fmodern\\fcharset0 Courier New;}"
                    "{\\f1\\fswiss\\fcharset0 Arial;}}\n{\\colortbl;";
  for (const Style& s : styles) {
    out += std::format("\\red{}\\green{}\\blue{};", hex_component(s.light, 0), hex_component(s.light, 2),
                       hex_component(s.light, 4));
  }
  out += "}\n";
  for (const Segment& segment : segments_of(document, ctx)) {
    CVT_TRY(ctx.checkpoint(0.5));
    if (!segment.code) {
      out += segment.heading > 0 ? std::format("{{\\pard\\f1\\b\\fs{} ", half_points + 8) : std::format("{{\\pard\\f1\\fs{} ", half_points + 2);
      rtf_escape(out, segment.text);
      out += "\\par}\n";
      continue;
    }
    out += std::format("{{\\pard\\f0\\fs{} ", half_points);
    std::size_t line = 1;
    if (numbers) out += std::format("{{\\cf6 {:>4} }}", line);
    for (const Token& token : tokenize(segment.text, segment.language)) {
      std::string_view text = std::string_view(segment.text).substr(token.offset, token.length);
      const Style& style = styles[index_of(token.cls)];
      while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view piece = text.substr(0, nl);
        if (!piece.empty()) {
          out += std::format("{{\\cf{}{}{} ", index_of(token.cls) + 1, style.bold ? "\\b" : "", style.italic ? "\\i" : "");
          rtf_escape(out, piece);
          out += "}";
        }
        if (nl == std::string_view::npos) break;
        out += "\\line\n";
        if (numbers) out += std::format("{{\\cf6 {:>4} }}", ++line);
        text.remove_prefix(nl + 1);
      }
    }
    out += "\\par}\n";
  }
  out += "}\n";
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

// ---------------------------------------------------------------------------- ANSI

std::string sgr_for(TC cls, std::string_view palette) {
  const Style& style = styles[index_of(cls)];
  if (cls == TC::Plain || cls == TC::Operator) return {};
  if (palette == "16") return style.sgr16.empty() ? std::string{} : std::format("\x1b[{}m", style.sgr16);
  std::string prefix;
  if (style.bold) prefix += "1;";
  if (style.italic) prefix += "3;";
  if (palette == "256") return std::format("\x1b[{}38;5;{}m", prefix, style.xterm256);
  return std::format("\x1b[{}38;2;{};{};{}m", prefix, hex_component(style.dark, 0), hex_component(style.dark, 2),
                     hex_component(style.dark, 4));
}

Result<Bytes> write_ansi(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& document = value.get<ir::Document>();
  const std::string palette = ctx.options.get_enum("colors", "16", {"16", "256", "truecolor"});
  const bool numbers = ctx.options.get_bool("line_numbers", false);
  std::string out;
  bool first = true;
  for (const Segment& segment : segments_of(document, ctx)) {
    CVT_TRY(ctx.checkpoint(0.5));
    if (!first) out += '\n';
    first = false;
    if (!segment.code) {
      out += segment.heading > 0 ? "\x1b[1m" + segment.text + "\x1b[0m\n" : segment.text + "\n";
      continue;
    }
    std::size_t line = 1;
    const auto number = [&] {
      if (numbers) out += std::format("\x1b[2m{:>4} \x1b[0m", line++);
    };
    number();
    for (const Token& token : tokenize(segment.text, segment.language)) {
      std::string_view text = std::string_view(segment.text).substr(token.offset, token.length);
      const std::string start = sgr_for(token.cls, palette);
      while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view piece = text.substr(0, nl);
        if (!piece.empty()) {
          out += start;
          // Control characters other than tab would act on the terminal: show them as ^X.
          for (const char c : piece) {
            const auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && c != '\t') || u == 0x7F) {
              out += '^';
              out.push_back(static_cast<char>(u ^ 0x40));
            } else {
              out.push_back(c);
            }
          }
          if (!start.empty()) out += "\x1b[0m";
        }
        if (nl == std::string_view::npos) break;
        out += '\n';
        number();
        text.remove_prefix(nl + 1);
      }
    }
    out += '\n';
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

// ---------------------------------------------------------------------------- ANSI input

/// Removes CSI (ESC [ ... final), OSC (ESC ] ... BEL / ESC \), two-byte
/// escapes and the 8-bit CSI 0x9B form; keeps everything else.
std::string strip_ansi(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c != '\x1b') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= text.size()) break;
    const char kind = text[i + 1];
    if (kind == '[') {
      std::size_t k = i + 2;
      while (k < text.size() && (static_cast<unsigned char>(text[k]) < 0x40 || static_cast<unsigned char>(text[k]) > 0x7E)) ++k;
      i = k;
    } else if (kind == ']' || kind == 'P' || kind == '_' || kind == '^') {
      std::size_t k = i + 2;
      while (k < text.size() && text[k] != '\x07' && !(text[k] == '\x1b' && k + 1 < text.size() && text[k + 1] == '\\')) ++k;
      i = (k < text.size() && text[k] == '\x1b') ? k + 1 : k;
    } else {
      i += 1;  // ESC + one character (charset selection, keypad modes ...)
    }
  }
  return out;
}

int sniff_ansi(ByteView data, std::string_view filename) {
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 8192)));
  const bool extension = util::iends_with(filename, ".ansi") || util::iends_with(filename, ".ans");
  const std::size_t escapes = [&text] {
    std::size_t n = 0;
    for (std::size_t at = text.find("\x1b["); at != std::string_view::npos; at = text.find("\x1b[", at + 2)) ++n;
    return n;
  }();
  if (escapes == 0) return extension ? 70 : 0;
  // Coloured logs: printable text with SGR sequences.
  std::string stripped = strip_ansi(text);
  if (!util::looks_like_text(as_bytes(stripped))) return 0;
  return extension ? 90 : (escapes >= 2 && text.find("m") != std::string_view::npos ? 72 : 0);
}

Result<std::string> read_ansi_text(ReadContext& ctx) {
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  const util::EncodingGuess guess = util::guess_encoding(ctx.input);
  return strip_ansi(util::decode_text(ctx.input, guess.encoding).text);
}

std::vector<OptionSpec> common_options(bool rtf) {
  std::vector<OptionSpec> specs = {
      OptionSpec::text("language", "Language", "auto",
                       "Language used for highlighting, e.g. py or cpp; 'auto' uses the source format."),
      OptionSpec::boolean("line_numbers", "Line numbers", false, "Number the lines of code."),
  };
  if (rtf) specs.push_back(OptionSpec::integer("font_size", "Font size", 10, 6, 72, "Font size in points."));
  return specs;
}

}  // namespace


void register_highlight(Registry& registry) {
  const auto add = [&registry](std::string id, std::string name, std::string description, std::vector<std::string> ext,
                               std::string mime, int popularity) {
    FormatInfo info;
    info.id = std::move(id);
    info.name = std::move(name);
    info.description = std::move(description);
    info.category = Category::Code;
    info.extensions = std::move(ext);
    info.mime = std::move(mime);
    info.popularity = popularity;
    registry.add_format(std::move(info));
  };

  add("html-highlight", "Syntax-highlighted HTML", "Source code as a web page with coloured syntax and light/dark themes.",
      {"html", "htm"}, "text/html", 45);
  Codec html;
  html.format_id = "html-highlight";
  html.kind = Kind::Document;
  html.write = write_html;
  html.write_cost = 12;
  html.write_options = common_options(false);
  html.write_options.push_back(OptionSpec::choice("theme", "Theme", "auto", {"auto", "light", "dark"},
                                                  "Colours: follow the reader's system setting, or always light/dark."));
  html.write_options.push_back(OptionSpec::boolean("standalone", "Complete page", true,
                                                   "Write a whole HTML page; off writes only the <pre> blocks.")
                                   .advanced());
  html.write_options.push_back(OptionSpec::integer("tab_size", "Tab width", 4, 1, 16, "Columns per tab character.").advanced());
  registry.add_codec(std::move(html));

  add("rtf-highlight", "Syntax-highlighted RTF", "Source code as rich text with coloured syntax for word processors.",
      {"rtf"}, "application/rtf", 25);
  Codec rtf;
  rtf.format_id = "rtf-highlight";
  rtf.kind = Kind::Document;
  rtf.write = write_rtf;
  rtf.write_cost = 12;
  rtf.write_options = common_options(true);
  registry.add_codec(std::move(rtf));

  FormatInfo ansi;
  ansi.id = "ansi";
  ansi.name = "ANSI-coloured text";
  ansi.description = "Text with terminal colour escape sequences, such as coloured logs or highlighted code.";
  ansi.category = Category::Code;
  ansi.extensions = {"ansi", "ans"};
  ansi.mime = "text/x-ansi";
  ansi.popularity = 20;
  ansi.sniff = sniff_ansi;
  registry.add_format(std::move(ansi));
  Codec ansi_document;
  ansi_document.format_id = "ansi";
  ansi_document.kind = Kind::Document;
  ansi_document.write = write_ansi;
  ansi_document.read = [](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(std::string text, read_ansi_text(ctx));
    text = util::convert_line_endings(text, util::LineEnding::Lf);
    if (text.ends_with('\n')) text.pop_back();
    ir::Document document;
    document.metadata.title = ctx.filename;
    document.blocks.push_back(ir::CodeBlock{"", std::move(text)});
    return document;
  };
  ansi_document.write_options = common_options(false);
  ansi_document.write_options.push_back(OptionSpec::choice(
      "colors", "Colours", "16", {"16", "256", "truecolor"},
      "Terminal palette: the 16 basic colours, the 256-colour palette, or 24-bit colour."));
  registry.add_codec(std::move(ansi_document));
  Codec ansi_text;
  ansi_text.format_id = "ansi";
  ansi_text.kind = Kind::PlainText;
  ansi_text.read = [](ReadContext& ctx) -> Result<AnyIR> {
    ir::PlainText out;
    CVT_TRY_ASSIGN(out.text, read_ansi_text(ctx));
    out.source_encoding = "utf-8";
    return out;
  };
  registry.add_codec(std::move(ansi_text));
}

}  // namespace cvt::code
