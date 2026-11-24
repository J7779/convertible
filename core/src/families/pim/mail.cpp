// Convertible core - pim family: e-mail messages.
//
//   eml    RFC 5322 / MIME message (.eml)
//   mbox   Unix mailbox (mboxo / mboxrd), a sequence of messages
//   emlx   Apple Mail message (byte count, RFC 5322 message, property list)
//
// Messages read into a Document (a header summary, then the body: the
// text/plain part, or the text/html part through the registry's "html"
// Document reader when one is registered, else the HTML with tags removed)
// and into an Archive of their attachments (mbox: an Archive of .eml files).
// MIME support: header unfolding, RFC 2047 encoded words (B and Q), RFC 2231
// parameters (filename*=UTF-8''...), multipart/* with nesting limits,
// quoted-printable and base64 transfer encodings, charsets UTF-8, US-ASCII,
// ISO-8859-1, Windows-1252 and UTF-16 (others are read as Windows-1252 with
// a warning). Outlook .msg files are not handled here.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/pim/pim.hpp"
#include "ir/archive.hpp"
#include "ir/document.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::pim {

namespace {

constexpr int max_mime_depth = 32;
constexpr std::size_t max_parts = 10'000;

// ============================================================================ text decoding

struct Decoder {
  const Context* ctx = nullptr;
  bool unknown_charset = false;

  std::string decode(ByteView bytes, std::string_view charset) {
    const std::string label = util::to_lower(util::trim(charset));
    if (label.empty() || label == "us-ascii" || label == "ascii") {
      return util::is_valid_utf8(as_chars(bytes)) ? std::string(as_chars(bytes))
                                                  : util::decode_text(bytes, util::TextEncoding::Windows1252).text;
    }
    if (const auto encoding = util::parse_encoding(label)) {
      std::string text = util::decode_text(bytes, *encoding).text;
      if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
      return text;
    }
    if (label == "iso-8859-15" || label == "latin-9" || label == "latin9") {
      return util::decode_text(bytes, util::TextEncoding::Windows1252).text;
    }
    unknown_charset = true;
    if (util::is_valid_utf8(as_chars(bytes))) return std::string(as_chars(bytes));
    return util::decode_text(bytes, util::TextEncoding::Windows1252).text;
  }
};

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

Bytes decode_quoted_printable(std::string_view text, bool header) {
  Bytes out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=' && i + 1 < text.size()) {
      if (text[i + 1] == '\r' || text[i + 1] == '\n') {  // soft line break
        i += (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n') ? 2 : 1;
        continue;
      }
      if (i + 2 < text.size()) {
        const int high = hex_value(text[i + 1]);
        const int low = hex_value(text[i + 2]);
        if (high >= 0 && low >= 0) {
          out.push_back(static_cast<std::byte>(high * 16 + low));
          i += 2;
          continue;
        }
      }
    }
    if (header && c == '_') {
      out.push_back(std::byte{' '});
      continue;
    }
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

Bytes decode_base64_lenient(std::string_view text) {
  std::string clean;
  clean.reserve(text.size());
  for (const char c : text) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '/') clean.push_back(c);
  }
  while (clean.size() % 4 != 0) clean.push_back('=');
  if (auto decoded = util::base64_decode(clean)) return std::move(*decoded);
  // Drop a dangling partial quantum and try once more.
  clean.resize(clean.size() / 4 * 4);
  while (!clean.empty() && clean.back() == '=') clean.pop_back();
  clean.resize(clean.size() / 4 * 4);
  return util::base64_decode(clean).value_or(Bytes{});
}

/// RFC 2047 encoded words in a header value.
std::string decode_header(std::string_view value, Decoder& decoder) {
  std::string out;
  std::size_t i = 0;
  bool last_was_word = false;
  std::string pending_space;
  while (i < value.size()) {
    if (value.substr(i).starts_with("=?")) {
      const std::size_t q1 = value.find('?', i + 2);
      const std::size_t q2 = q1 == std::string_view::npos ? q1 : value.find('?', q1 + 1);
      const std::size_t end = q2 == std::string_view::npos ? q2 : value.find("?=", q2 + 1);
      if (end != std::string_view::npos && q2 == q1 + 2) {
        std::string_view charset = value.substr(i + 2, q1 - i - 2);
        if (const std::size_t star = charset.find('*'); star != std::string_view::npos) charset = charset.substr(0, star);  // RFC 2231 language
        const char encoding = util::ascii_upper(value[q1 + 1]);
        const std::string_view payload = value.substr(q2 + 1, end - q2 - 1);
        const Bytes bytes = encoding == 'B' ? decode_base64_lenient(payload) : decode_quoted_printable(payload, true);
        // White space between two encoded words is dropped.
        if (!last_was_word) out += pending_space;
        pending_space.clear();
        out += decoder.decode(bytes, charset);
        last_was_word = true;
        i = end + 2;
        continue;
      }
    }
    const char c = value[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pending_space.push_back(' ');
      ++i;
      continue;
    }
    out += pending_space;
    pending_space.clear();
    out.push_back(c);
    last_was_word = false;
    ++i;
  }
  if (!last_was_word) out += pending_space;
  // Raw 8-bit headers: treat as UTF-8 when valid, else Windows-1252.
  if (!util::is_valid_utf8(out)) out = util::decode_text(as_bytes(out), util::TextEncoding::Windows1252).text;
  return std::string(util::trim(out));
}

// ============================================================================ MIME structure

struct Header {
  std::string name;   // lower case
  std::string value;  // unfolded, still encoded
};

struct Part {
  std::vector<Header> headers;
  ByteView body;
  std::vector<Part> children;
  std::optional<std::vector<Part>> nested_message;  // message/rfc822 content, as one part
  std::string type = "text/plain";
  std::map<std::string, std::string> params;  // lower-case names, RFC 2231 decoded

  [[nodiscard]] std::string header(std::string_view name) const {
    for (const Header& h : headers) {
      if (h.name == name) return h.value;
    }
    return {};
  }
};

/// "type/subtype; a=b; c="d"" -> type and parameters (RFC 2231 aware).
std::pair<std::string, std::map<std::string, std::string>> parse_parameters(std::string_view value, Decoder& decoder) {
  std::vector<std::string> pieces;
  {
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
      const char c = value[i];
      if (c == '"') quoted = !quoted;
      if (c == '\\' && quoted && i + 1 < value.size()) {
        current.push_back(value[++i]);
        continue;
      }
      if (c == ';' && !quoted) {
        pieces.push_back(std::move(current));
        current.clear();
        continue;
      }
      current.push_back(c);
    }
    pieces.push_back(std::move(current));
  }
  std::string type = util::to_lower(util::trim(pieces[0]));
  std::map<std::string, std::string> params;
  std::map<std::string, std::map<int, std::pair<bool, std::string>>> continued;  // name -> index -> (encoded, text)
  for (std::size_t p = 1; p < pieces.size(); ++p) {
    const std::size_t eq = pieces[p].find('=');
    if (eq == std::string::npos) continue;
    std::string name = util::to_lower(util::trim(std::string_view(pieces[p]).substr(0, eq)));
    std::string text(util::trim(std::string_view(pieces[p]).substr(eq + 1)));
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
    bool encoded = false;
    if (name.ends_with('*')) {
      encoded = true;
      name.pop_back();
    }
    const std::size_t star = name.find('*');
    if (star != std::string::npos) {
      const int index = static_cast<int>(util::parse_int(std::string_view(name).substr(star + 1)).value_or(0));
      if (index >= 0 && index < 1000) continued[name.substr(0, star)][index] = {encoded, text};
      continue;
    }
    if (encoded) {
      continued[name][0] = {true, text};
      continue;
    }
    params[name] = text;
  }
  for (const auto& [name, segments] : continued) {
    std::string charset;
    std::string bytes;
    bool first = true;
    for (const auto& [index, segment] : segments) {
      std::string piece = segment.second;
      if (segment.first) {
        if (first) {
          // charset'language'value
          const std::size_t a = piece.find('\'');
          const std::size_t b = a == std::string::npos ? a : piece.find('\'', a + 1);
          if (b != std::string::npos) {
            charset = piece.substr(0, a);
            piece = piece.substr(b + 1);
          }
        }
        std::string decoded;
        for (std::size_t i = 0; i < piece.size(); ++i) {
          if (piece[i] == '%' && i + 2 < piece.size() && hex_value(piece[i + 1]) >= 0 && hex_value(piece[i + 2]) >= 0) {
            decoded.push_back(static_cast<char>(hex_value(piece[i + 1]) * 16 + hex_value(piece[i + 2])));
            i += 2;
          } else {
            decoded.push_back(piece[i]);
          }
        }
        piece = std::move(decoded);
      }
      bytes += piece;
      first = false;
    }
    params[name] = decoder.decode(as_bytes(bytes), charset.empty() ? "utf-8" : charset);
  }
  return {type, params};
}

/// Split "headers CRLF CRLF body".
std::pair<std::vector<Header>, ByteView> split_message(ByteView data) {
  const std::string_view text = as_chars(data);
  std::vector<Header> headers;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    const std::size_t line_end = end == std::string_view::npos ? text.size() : end;
    std::string_view line = text.substr(pos, line_end - pos);
    if (line.ends_with('\r')) line.remove_suffix(1);
    pos = end == std::string_view::npos ? text.size() : end + 1;
    if (line.empty()) break;
    if ((line[0] == ' ' || line[0] == '\t') && !headers.empty()) {
      headers.back().value += " " + std::string(util::trim(line));
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      // Not a header: the message has no header block (or it ended early).
      if (headers.empty()) {
        pos = 0;
        break;
      }
      continue;
    }
    if (headers.size() < 10000) headers.push_back({util::to_lower(util::trim(line.substr(0, colon))), std::string(util::trim(line.substr(colon + 1)))});
  }
  return {std::move(headers), data.subspan(std::min(pos, data.size()))};
}

Result<Part> parse_part(ByteView data, Decoder& decoder, int depth, std::size_t& count);

Result<std::vector<Part>> split_multipart(ByteView body, const std::string& boundary, Decoder& decoder, int depth,
                                          std::size_t& count) {
  std::vector<Part> parts;
  const std::string_view text = as_chars(body);
  const std::string delimiter = "--" + boundary;
  std::size_t pos = 0;
  std::optional<std::size_t> part_start;
  while (pos <= text.size()) {
    const std::size_t end = text.find('\n', pos);
    const std::size_t line_end = end == std::string_view::npos ? text.size() : end;
    std::string_view line = text.substr(pos, line_end - pos);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.starts_with(delimiter)) {
      const std::string_view rest = util::trim_right(line.substr(delimiter.size()));
      if (rest.empty() || rest == "--") {
        if (part_start) {
          // The line break before the delimiter belongs to the delimiter.
          std::size_t stop = pos;
          if (stop > *part_start && text[stop - 1] == '\n') --stop;
          if (stop > *part_start && text[stop - 1] == '\r') --stop;
          CVT_TRY_ASSIGN(Part part, parse_part(body.subspan(*part_start, stop - *part_start), decoder, depth + 1, count));
          parts.push_back(std::move(part));
        }
        if (rest == "--") break;
        part_start = end == std::string_view::npos ? text.size() : end + 1;
      }
    }
    if (end == std::string_view::npos) break;
    pos = end + 1;
  }
  if (part_start && *part_start < text.size()) {
    // A missing closing delimiter: the rest of the body is the last part.
    const bool closed = text.find(delimiter + "--") != std::string_view::npos;
    if (!closed) {
      CVT_TRY_ASSIGN(Part part, parse_part(body.subspan(*part_start), decoder, depth + 1, count));
      parts.push_back(std::move(part));
    }
  }
  return parts;
}

Result<Part> parse_part(ByteView data, Decoder& decoder, int depth, std::size_t& count) {
  if (depth > max_mime_depth) return fail(Status::Limit, "MIME parts are nested too deeply");
  if (++count > max_parts) return fail(Status::Limit, "the message has too many MIME parts");
  Part part;
  auto [headers, body] = split_message(data);
  part.headers = std::move(headers);
  part.body = body;
  const std::string content_type = part.header("content-type");
  if (!content_type.empty()) {
    auto [type, params] = parse_parameters(content_type, decoder);
    if (type.find('/') != std::string::npos) part.type = type;
    part.params = std::move(params);
  }
  if (part.type.starts_with("multipart/") && part.params.contains("boundary") && !part.params["boundary"].empty()) {
    CVT_TRY_ASSIGN(part.children, split_multipart(body, part.params["boundary"], decoder, depth, count));
  } else if (part.type == "message/rfc822") {
    CVT_TRY_ASSIGN(Part nested, parse_part(body, decoder, depth + 1, count));
    part.nested_message = std::vector<Part>{};
    part.nested_message->push_back(std::move(nested));
  }
  return part;
}

/// The transfer-decoded content of a leaf part.
Result<Bytes> part_content(const Part& part, const Context& ctx, std::uint64_t& total) {
  const std::string encoding = util::to_lower(util::trim(part.header("content-transfer-encoding")));
  Bytes out;
  if (encoding == "base64") out = decode_base64_lenient(as_chars(part.body));
  else if (encoding == "quoted-printable") out = decode_quoted_printable(as_chars(part.body), false);
  else out.assign(part.body.begin(), part.body.end());
  total += out.size();
  CVT_TRY(ctx.limits.check_decompressed(total));
  return out;
}

std::string filename_of(const Part& part, Decoder& decoder) {
  const auto [disposition, params] = parse_parameters(part.header("content-disposition"), decoder);
  std::string name;
  if (const auto it = params.find("filename"); it != params.end()) name = it->second;
  if (name.empty()) {
    if (const auto it = part.params.find("name"); it != part.params.end()) name = it->second;
  }
  return decode_header(name, decoder);
}

bool is_attachment(const Part& part, Decoder& decoder) {
  const auto [disposition, params] = parse_parameters(part.header("content-disposition"), decoder);
  if (disposition == "attachment") return true;
  if (part.type == "message/rfc822") return true;
  if (part.type.starts_with("text/") && disposition != "inline") return !filename_of(part, decoder).empty();
  if (part.type.starts_with("multipart/")) return false;
  return !filename_of(part, decoder).empty() || !part.type.starts_with("text/");
}

struct Bodies {
  const Part* plain = nullptr;
  const Part* html = nullptr;
  std::vector<const Part*> attachments;
};

void collect(const Part& part, Decoder& decoder, Bodies& bodies, int depth) {
  if (depth > max_mime_depth) return;
  if (!part.children.empty()) {
    for (const Part& child : part.children) collect(child, decoder, bodies, depth + 1);
    return;
  }
  if (is_attachment(part, decoder)) {
    bodies.attachments.push_back(&part);
    return;
  }
  if (part.type == "text/plain" && bodies.plain == nullptr) bodies.plain = &part;
  else if (part.type == "text/html" && bodies.html == nullptr) bodies.html = &part;
  else if (part.type.starts_with("text/") && bodies.plain == nullptr) bodies.plain = &part;
}

// ============================================================================ Document

struct Entity {
  std::string_view name;
  char32_t code_point;
};
constexpr Entity html_entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4}, {"Aring", 0xC5},
    {"AElig", 0xC6}, {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA}, {"Euml", 0xCB},
    {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF}, {"ETH", 0xD0}, {"Ntilde", 0xD1},
    {"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6}, {"Oslash", 0xD8},
    {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC}, {"Yacute", 0xDD}, {"THORN", 0xDE},
    {"szlig", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4},
    {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA},
    {"euml", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"eth", 0xF0},
    {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6},
    {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD},
    {"thorn", 0xFE}, {"yuml", 0xFF}, {"copy", 0xA9}, {"reg", 0xAE}, {"euro", 0x20AC}, {"pound", 0xA3},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
};

std::string strip_html(std::string_view html) {
  std::string out;
  std::size_t i = 0;
  bool skip = false;  // inside <script> / <style>
  while (i < html.size()) {
    if (html[i] == '<') {
      const std::size_t close = html.find('>', i);
      if (close == std::string_view::npos) break;
      const std::string tag = util::to_lower(html.substr(i + 1, std::min<std::size_t>(close - i - 1, 7)));
      if (tag.starts_with("script") || tag.starts_with("style")) skip = true;
      if (tag.starts_with("/script") || tag.starts_with("/style")) skip = false;
      if (tag.starts_with("br") || tag.starts_with("p") || tag.starts_with("/p") || tag.starts_with("div") ||
          tag.starts_with("/div") || tag.starts_with("li") || tag.starts_with("tr") || tag.starts_with("h")) {
        out.push_back('\n');
      }
      i = close + 1;
      continue;
    }
    if (skip) {
      ++i;
      continue;
    }
    if (html[i] == '&') {
      const std::size_t semicolon = html.find(';', i);
      if (semicolon != std::string_view::npos && semicolon - i <= 8) {
        const std::string_view name = html.substr(i + 1, semicolon - i - 1);
        std::optional<std::int64_t> code;
        for (const Entity& entity : html_entities) {
          if (entity.name == name) code = entity.code_point;
        }
        if (name.starts_with("#x") || name.starts_with("#X")) code = util::parse_int(name.substr(2), 16);
        else if (name.starts_with('#')) code = util::parse_int(name.substr(1));
        if (code && *code > 0 && *code <= 0x10FFFF) {
          util::append_utf8(out, static_cast<char32_t>(*code));
          i = semicolon + 1;
          continue;
        }
      }
    }
    out.push_back(html[i] == '\r' ? ' ' : html[i]);
    ++i;
  }
  return out;
}

void text_paragraphs(std::string_view text, ir::Blocks& blocks) {
  std::string paragraph;
  const auto flush = [&] {
    if (!paragraph.empty()) blocks.push_back(ir::Paragraph{{ir::Text{paragraph}}});
    paragraph.clear();
  };
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim_right(raw);
    if (util::trim(line).empty()) {
      flush();
      continue;
    }
    if (!paragraph.empty()) paragraph.push_back('\n');
    paragraph.append(line);
  }
  flush();
}

std::string human_size(std::size_t bytes) {
  if (bytes < 1024) return std::format("{} bytes", bytes);
  if (bytes < 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
  return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

Result<ir::Document> message_document(const Part& root, Decoder& decoder, const ReadContext& ctx, std::uint64_t& total) {
  using namespace ir::build;
  ir::Document document;
  const std::string subject = decode_header(root.header("subject"), decoder);
  const std::string from = decode_header(root.header("from"), decoder);
  const std::string date_text = root.header("date");
  document.metadata.title = subject;
  document.metadata.author = from;
  if (const auto date = parse_rfc822_datetime(date_text)) {
    document.metadata.date = format_iso_datetime(date->utc_seconds(), date->offset_minutes.value_or(0));
  }
  document.blocks.push_back(heading(1, subject.empty() ? std::string("(no subject)") : subject));
  std::vector<ir::TableRow> rows;
  for (const auto& [name, label] : {std::pair{"from", "From"}, std::pair{"to", "To"}, std::pair{"cc", "Cc"},
                                    std::pair{"date", "Date"}, std::pair{"subject", "Subject"}}) {
    const std::string value = decode_header(root.header(name), decoder);
    if (!value.empty()) rows.push_back(row({label, value}));
  }
  if (!rows.empty()) document.blocks.push_back(table(std::move(rows)));

  Bodies bodies;
  collect(root, decoder, bodies, 0);
  if (bodies.plain != nullptr) {
    CVT_TRY_ASSIGN(const Bytes content, part_content(*bodies.plain, ctx, total));
    const auto charset = bodies.plain->params.find("charset");
    text_paragraphs(decoder.decode(content, charset == bodies.plain->params.end() ? "" : charset->second), document.blocks);
  } else if (bodies.html != nullptr) {
    CVT_TRY_ASSIGN(const Bytes content, part_content(*bodies.html, ctx, total));
    const auto charset = bodies.html->params.find("charset");
    const std::string html = decoder.decode(content, charset == bodies.html->params.end() ? "" : charset->second);
    bool rendered = false;
    if (ctx.registry != nullptr) {
      for (const Codec* codec : ctx.registry->codecs_for("html")) {
        if (codec->kind != Kind::Document || !codec->read) continue;
        ReadContext nested;
        nested.input = as_bytes(html);
        nested.format_id = "html";
        nested.limits = ctx.limits;
        nested.registry = ctx.registry;
        nested.warnings = ctx.warnings;
        if (auto options = Options().normalized(codec->read_options)) nested.options = std::move(*options);
        auto value = codec->read(nested);
        if (value && value->is<ir::Document>()) {
          ir::Document body = std::move(*value).get<ir::Document>();
          for (ir::Block& block : body.blocks) document.blocks.push_back(std::move(block));
          rendered = true;
        }
        break;
      }
    }
    if (!rendered) {
      ctx.warn("The message has only an HTML body; it was converted to plain text without formatting.");
      text_paragraphs(strip_html(html), document.blocks);
    }
  }
  if (!bodies.attachments.empty()) {
    std::vector<ir::Blocks> items;
    for (const Part* attachment : bodies.attachments) {
      CVT_TRY_ASSIGN(const Bytes content, part_content(*attachment, ctx, total));
      std::string name = filename_of(*attachment, decoder);
      if (name.empty() && attachment->nested_message) name = decode_header(attachment->nested_message->front().header("subject"), decoder) + ".eml";
      if (name.empty()) name = "attachment";
      items.push_back({paragraph(std::format("{} ({}, {})", name, attachment->type, human_size(content.size())))});
    }
    document.blocks.push_back(heading(2, "Attachments"));
    document.blocks.push_back(list(false, std::move(items)));
  }
  return document;
}

// ============================================================================ Archive

std::string extension_for(std::string_view type) {
  if (type == "image/png") return ".png";
  if (type == "image/jpeg") return ".jpg";
  if (type == "image/gif") return ".gif";
  if (type == "application/pdf") return ".pdf";
  if (type == "text/plain") return ".txt";
  if (type == "text/html") return ".html";
  if (type == "text/calendar") return ".ics";
  if (type == "message/rfc822") return ".eml";
  return ".bin";
}

std::string unique_name(std::string name, std::map<std::string, int>& used) {
  const auto it = used.find(name);
  if (it == used.end()) {
    used[name] = 1;
    return name;
  }
  const std::size_t dot = name.rfind('.');
  const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
  const std::string extension = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
  std::string candidate;
  do {
    candidate = std::format("{} ({}){}", stem, ++it->second, extension);
  } while (used.contains(candidate));
  used[candidate] = 1;
  return candidate;
}

Result<void> add_attachments(const Part& root, Decoder& decoder, const ReadContext& ctx, std::uint64_t& total,
                             ir::Archive& archive, std::map<std::string, int>& used, std::string_view prefix) {
  Bodies bodies;
  collect(root, decoder, bodies, 0);
  for (const Part* attachment : bodies.attachments) {
    CVT_TRY(ctx.limits.check_archive_entries(archive.entries.size() + 1));
    CVT_TRY_ASSIGN(Bytes content, part_content(*attachment, ctx, total));
    std::string name = filename_of(*attachment, decoder);
    if (name.empty()) name = std::format("attachment-{}{}", archive.entries.size() + 1, extension_for(attachment->type));
    if (const std::size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    std::string safe = ir::sanitize_archive_path(name).value_or(std::string());
    // Attachments live in one folder: keep only the final path component.
    if (const std::size_t slash = safe.rfind('/'); slash != std::string::npos) safe = safe.substr(slash + 1);
    if (safe.empty()) safe = std::format("attachment-{}{}", archive.entries.size() + 1, extension_for(attachment->type));
    ir::ArchiveEntry entry;
    entry.path = std::string(prefix) + unique_name(safe, used);
    entry.bytes = std::move(content);
    archive.entries.push_back(std::move(entry));
  }
  return {};
}

// ============================================================================ containers

/// Messages of an mbox file (byte ranges), with mboxrd ">From " unescaping.
std::vector<Bytes> mbox_messages(ByteView data) {
  std::vector<Bytes> messages;
  const std::string_view text = as_chars(data);
  std::size_t pos = 0;
  std::optional<Bytes> current;
  bool previous_blank = true;
  while (pos < text.size()) {
    const std::size_t end = text.find('\n', pos);
    const std::size_t next = end == std::string_view::npos ? text.size() : end + 1;
    const std::string_view line = text.substr(pos, next - pos);
    if (line.starts_with("From ") && previous_blank) {
      if (current) messages.push_back(std::move(*current));
      current = Bytes{};
    } else if (current) {
      std::string_view content = line;
      // ">From " (any number of '>') loses one '>'.
      std::size_t quotes = 0;
      while (quotes < content.size() && content[quotes] == '>') ++quotes;
      if (quotes > 0 && content.substr(quotes).starts_with("From ")) content.remove_prefix(1);
      cvt::append(*current, content);
    }
    const std::string_view stripped = util::trim_right(line);
    previous_blank = stripped.empty();
    pos = next;
  }
  if (current) messages.push_back(std::move(*current));
  // The blank line before each separator belongs to the mailbox, not the message.
  for (Bytes& message : messages) {
    if (!message.empty() && message.back() == std::byte{'\n'}) message.pop_back();
    if (!message.empty() && message.back() == std::byte{'\r'}) message.pop_back();
    message.push_back(std::byte{'\n'});
  }
  return messages;
}

/// The RFC 5322 message inside an .emlx file.
Result<ByteView> emlx_message(ByteView data) {
  const std::string_view text = as_chars(data);
  const std::size_t newline = text.find('\n');
  if (newline == std::string_view::npos || newline > 20) return fail(Status::Decode, "not an Apple Mail message (no byte count)");
  const auto length = util::parse_int(util::trim(text.substr(0, newline)));
  if (!length || *length < 0 || static_cast<std::uint64_t>(*length) > data.size() - newline - 1) {
    return fail(Status::Decode, "the Apple Mail byte count does not match the file");
  }
  return data.subspan(newline + 1, static_cast<std::size_t>(*length));
}

// ============================================================================ registration

Result<Part> parse_root(ByteView data, Decoder& decoder) {
  std::size_t count = 0;
  CVT_TRY_ASSIGN(Part root, parse_part(data, decoder, 0, count));
  if (root.headers.empty()) return fail(Status::Decode, "not an e-mail message (no header lines)");
  bool known = false;
  for (const Header& header : root.headers) {
    known = known || header.name == "from" || header.name == "to" || header.name == "subject" || header.name == "date" ||
            header.name == "message-id" || header.name == "mime-version" || header.name == "received";
  }
  if (!known) return fail(Status::Decode, "not an e-mail message (no From, To, Subject or Date header)");
  return root;
}

void warn_charsets(const Decoder& decoder, const Context& ctx) {
  if (decoder.unknown_charset) ctx.warn("Some text used an unsupported character set and was read as Windows-1252.");
}

Result<AnyIR> read_document(std::string_view format, ReadContext& ctx) {
  Decoder decoder{&ctx};
  std::uint64_t total = 0;
  if (format == "mbox") {
    ir::Document combined;
    const std::vector<Bytes> messages = mbox_messages(ctx.input);
    if (messages.empty()) return fail(Status::Decode, "not a mailbox (no 'From ' separator line)");
    CVT_TRY(ctx.limits.check_archive_entries(messages.size()));
    for (std::size_t i = 0; i < messages.size(); ++i) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(messages.size())));
      CVT_TRY_ASSIGN(const Part root, parse_root(messages[i], decoder));
      CVT_TRY_ASSIGN(ir::Document document, message_document(root, decoder, ctx, total));
      if (i > 0) combined.blocks.push_back(ir::HorizontalRule{});
      for (ir::Block& block : document.blocks) combined.blocks.push_back(std::move(block));
    }
    warn_charsets(decoder, ctx);
    return combined;
  }
  ByteView message = ctx.input;
  if (format == "emlx") {
    CVT_TRY_ASSIGN(message, emlx_message(ctx.input));
  }
  CVT_TRY_ASSIGN(const Part root, parse_root(message, decoder));
  CVT_TRY_ASSIGN(ir::Document document, message_document(root, decoder, ctx, total));
  warn_charsets(decoder, ctx);
  return document;
}

Result<AnyIR> read_archive(std::string_view format, ReadContext& ctx) {
  Decoder decoder{&ctx};
  std::uint64_t total = 0;
  ir::Archive archive;
  if (format == "mbox") {
    const std::vector<Bytes> messages = mbox_messages(ctx.input);
    if (messages.empty()) return fail(Status::Decode, "not a mailbox (no 'From ' separator line)");
    CVT_TRY(ctx.limits.check_archive_entries(messages.size()));
    std::map<std::string, int> used;
    for (std::size_t i = 0; i < messages.size(); ++i) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(messages.size())));
      // Validate each message, then store it untouched.
      CVT_TRY_ASSIGN(const Part root, parse_root(messages[i], decoder));
      std::string subject = decode_header(root.header("subject"), decoder);
      std::string stem;
      for (const char c : subject) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
          stem.push_back('_');
        } else {
          stem.push_back(c);
        }
      }
      if (util::utf8_length(stem) > 60) stem = std::string(util::utf32_to_utf8(util::utf8_to_utf32(stem).substr(0, 60)));
      stem = std::string(util::trim(stem));
      ir::ArchiveEntry entry;
      entry.path = unique_name(std::format("{:04} {}.eml", i + 1, stem.empty() ? std::string("message") : stem), used);
      entry.path = ir::sanitize_archive_path(entry.path).value_or(std::format("{:04}.eml", i + 1));
      entry.bytes = messages[i];
      total += entry.bytes.size();
      CVT_TRY(ctx.limits.check_decompressed(total));
      archive.entries.push_back(std::move(entry));
    }
    warn_charsets(decoder, ctx);
    return archive;
  }
  ByteView message = ctx.input;
  if (format == "emlx") {
    CVT_TRY_ASSIGN(message, emlx_message(ctx.input));
  }
  CVT_TRY_ASSIGN(const Part root, parse_root(message, decoder));
  std::map<std::string, int> used;
  CVT_TRY(add_attachments(root, decoder, ctx, total, archive, used, ""));
  warn_charsets(decoder, ctx);
  return archive;
}

int sniff_eml(ByteView data) {
  const std::string head(as_chars(data.first(std::min<std::size_t>(data.size(), 4096))));
  if (!util::looks_like_text(as_bytes(std::string_view(head).substr(0, 512)))) return 0;
  int score = 0;
  std::size_t pos = 0;
  int lines = 0;
  while (pos < head.size() && lines < 40) {
    const std::size_t end = head.find('\n', pos);
    const std::string_view line = std::string_view(head).substr(pos, (end == std::string::npos ? head.size() : end) - pos);
    pos = end == std::string::npos ? head.size() : end + 1;
    ++lines;
    if (util::trim(line).empty()) break;
    if (line[0] == ' ' || line[0] == '\t') continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return 0;
    const std::string name = util::to_lower(line.substr(0, colon));
    if (name.find(' ') != std::string::npos) return 0;
    if (name == "from" || name == "to" || name == "subject" || name == "date" || name == "message-id" ||
        name == "mime-version" || name == "received" || name == "return-path" || name == "delivered-to") {
      score += 20;
    }
  }
  return std::min(score, 90);
}

}  // namespace

void register_mail(Registry& registry) {
  struct Format {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> extensions;
    std::string mime;
    int popularity;
    std::function<int(ByteView, std::string_view)> sniff;
  };
  const Format formats[] = {
      {"eml", "E-mail message", "Internet e-mail message (RFC 5322 / MIME).", {"eml"}, "message/rfc822", 60,
       [](ByteView data, std::string_view) { return sniff_eml(data); }},
      {"mbox", "Mailbox", "Unix mbox file holding many e-mail messages.", {"mbox", "mbx"}, "application/mbox", 35,
       [](ByteView data, std::string_view) {
         const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 1024)));
         if (!head.starts_with("From ")) return 0;
         const std::size_t newline = head.find('\n');
         return newline != std::string_view::npos && sniff_eml(data.subspan(newline + 1)) >= 40 ? 92 : 0;
       }},
      {"emlx", "Apple Mail message", "Message file of Apple Mail (.emlx).", {"emlx"}, "message/rfc822", 15,
       [](ByteView data, std::string_view) {
         const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 64)));
         const std::size_t newline = head.find('\n');
         if (newline == std::string_view::npos || newline == 0 || newline > 12) return 0;
         const std::string_view count = util::trim(head.substr(0, newline));
         if (count.empty() || !std::ranges::all_of(count, [](char c) { return util::is_ascii_digit(c); })) return 0;
         return sniff_eml(data.subspan(newline + 1)) >= 40 ? 92 : 0;
       }},
  };
  for (const Format& format : formats) {
    FormatInfo info;
    info.id = format.id;
    info.name = format.name;
    info.description = format.description;
    info.category = Category::Document;
    info.extensions = format.extensions;
    info.mime = format.mime;
    info.popularity = format.popularity;
    info.sniff = format.sniff;
    registry.add_format(std::move(info));

    const std::string id = format.id;
    Codec document;
    document.format_id = id;
    document.kind = Kind::Document;
    document.lossy = true;
    document.read = [id](ReadContext& ctx) { return read_document(id, ctx); };
    registry.add_codec(std::move(document));

    Codec archive;
    archive.format_id = id;
    archive.kind = Kind::Archive;
    archive.read_cost = 20;
    archive.read = [id](ReadContext& ctx) { return read_archive(id, ctx); };
    registry.add_codec(std::move(archive));
  }
}

}  // namespace cvt::pim
