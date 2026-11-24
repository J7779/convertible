// Convertible core - vector family: DXF group code tokenizer (ASCII and
// binary), string code pages and the MTEXT inline format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "families/vector/cad_internal.hpp"
#include "families/vector/metafile_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

constexpr std::string_view binary_sentinel{"AutoCAD Binary DXF\r\n\x1a\0", 22};

enum class ValueType : std::uint8_t { String, Double, Int16, Int32, Int64, Bool, Binary };

ValueType value_type(int code) noexcept {
  if (code >= 0 && code <= 9) return ValueType::String;
  if (code >= 10 && code <= 59) return ValueType::Double;
  if (code >= 60 && code <= 79) return ValueType::Int16;
  if (code >= 90 && code <= 99) return ValueType::Int32;
  if (code >= 100 && code <= 109) return ValueType::String;
  if (code >= 110 && code <= 149) return ValueType::Double;
  if (code >= 160 && code <= 169) return ValueType::Int64;
  if (code >= 170 && code <= 179) return ValueType::Int16;
  if (code >= 210 && code <= 239) return ValueType::Double;
  if (code >= 270 && code <= 289) return ValueType::Int16;
  if (code >= 290 && code <= 299) return ValueType::Bool;
  if (code >= 310 && code <= 319) return ValueType::Binary;
  if (code >= 370 && code <= 389) return ValueType::Int16;
  if (code >= 400 && code <= 409) return ValueType::Int16;
  if (code >= 420 && code <= 429) return ValueType::Int32;
  if (code >= 440 && code <= 459) return ValueType::Int32;
  if (code >= 460 && code <= 469) return ValueType::Double;
  if (code == 1004) return ValueType::Binary;
  if (code >= 1010 && code <= 1059) return ValueType::Double;
  if (code >= 1060 && code <= 1070) return ValueType::Int16;
  if (code == 1071) return ValueType::Int32;
  return ValueType::String;
}

Result<std::vector<DxfTag>> tokenize_binary(ByteView data, const Context& ctx) {
  util::ByteReader reader(data);
  reader.skip(binary_sentinel.size());
  // R12 binary files use one-byte group codes (255 escapes a 16-bit code);
  // later versions always use 16-bit codes. The first tag is (0, "SECTION").
  const bool wide = data.size() > 23 && static_cast<std::uint8_t>(data[23]) == 0;
  std::vector<DxfTag> tags;
  while (!reader.at_end()) {
    if ((tags.size() & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(reader.position()) / data.size() * 0.3));
    int code = 0;
    if (wide) {
      code = reader.i16le();
    } else {
      code = reader.u8();
      if (code == 255) code = reader.i16le();
    }
    if (!reader.ok()) return fail(Status::Decode, "binary DXF is truncated");
    DxfTag tag;
    tag.code = code;
    switch (value_type(code)) {
      case ValueType::String: {
        const ByteView rest = reader.peek_rest();
        std::size_t end = 0;
        while (end < rest.size() && rest[end] != std::byte{0}) ++end;
        if (end == rest.size()) return fail(Status::Decode, "binary DXF string is not terminated");
        tag.text = as_chars(rest.first(end));
        reader.skip(end + 1);
        break;
      }
      case ValueType::Double: tag.number = reader.f64le(); tag.binary_number = true; break;
      case ValueType::Int16: tag.number = reader.i16le(); tag.binary_number = true; break;
      case ValueType::Int32: tag.number = reader.i32le(); tag.binary_number = true; break;
      case ValueType::Int64: tag.number = static_cast<double>(reader.i64le()); tag.binary_number = true; break;
      case ValueType::Bool: tag.number = reader.u8(); tag.binary_number = true; break;
      case ValueType::Binary: {
        const std::uint8_t length = reader.u8();
        tag.text = as_chars(reader.bytes(length));
        break;
      }
    }
    if (!reader.ok()) return fail(Status::Decode, "binary DXF is truncated");
    if (tag.binary_number && !std::isfinite(tag.number)) tag.number = 0.0;
    if (code == 999) continue;
    tags.push_back(tag);
    if (code == 0 && tag.text == "EOF") break;
  }
  return tags;
}

// Next line of an ASCII DXF without its line terminator (LF, CRLF or a lone CR).
std::optional<std::string_view> next_line(std::string_view text, std::size_t& pos) {
  if (pos >= text.size()) return std::nullopt;
  const std::size_t start = pos;
  while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') ++pos;
  const std::string_view line = text.substr(start, pos - start);
  if (pos < text.size()) {
    if (text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n') pos += 2;
    else ++pos;
  }
  return line;
}

Result<std::vector<DxfTag>> tokenize_ascii(ByteView data, const Context& ctx) {
  std::string_view text = as_chars(data);
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::vector<DxfTag> tags;
  std::size_t pos = 0;
  std::size_t line_number = 0;
  while (true) {
    if ((tags.size() & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / std::max<std::size_t>(text.size(), 1) * 0.3));
    const auto code_line = next_line(text, pos);
    if (!code_line) break;
    ++line_number;
    const std::string_view code_text = util::trim(*code_line);
    if (code_text.empty() && pos >= text.size()) break;  // trailing blank line
    const auto code = util::parse_int(code_text);
    if (!code || *code < -32768 || *code > 32767) {
      return fail(Status::Decode, "malformed DXF group code on line {}", line_number);
    }
    const auto value_line = next_line(text, pos);
    if (!value_line) return fail(Status::Decode, "DXF is truncated after line {}", line_number);
    ++line_number;
    if (*code == 999) continue;
    DxfTag tag;
    tag.code = static_cast<int>(*code);
    tag.text = *value_line;
    tags.push_back(tag);
    if (tag.code == 0 && util::trim(tag.text) == "EOF") break;
  }
  if (tags.empty()) return fail(Status::Decode, "the DXF file is empty");
  return tags;
}

int charset_of_codepage(std::string_view codepage) {
  const std::string cp = util::to_upper(util::trim(codepage));
  if (cp == "ANSI_1250") return 238;
  if (cp == "ANSI_1251") return 204;
  if (cp == "ANSI_1253") return 161;
  if (cp == "ANSI_1254") return 162;
  if (cp == "ANSI_1255") return 177;
  if (cp == "ANSI_1256") return 178;
  if (cp == "ANSI_1257") return 186;
  if (cp == "ANSI_1258") return 163;
  if (cp == "ANSI_874") return 222;
  if (cp == "DOS437") return 255;
  if (cp == "ANSI_932" || cp == "DOS932") return 128;
  if (cp == "ANSI_936") return 134;
  if (cp == "ANSI_949") return 129;
  if (cp == "ANSI_950") return 136;
  if (cp == "ANSI_1361") return 130;
  return 0;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

double tag_double(const DxfTag& tag) noexcept {
  if (tag.binary_number) return tag.number;
  const auto value = util::parse_double(util::trim(tag.text));
  return value && std::isfinite(*value) ? *value : 0.0;
}

std::int64_t tag_int(const DxfTag& tag) noexcept {
  double value = 0;
  if (tag.binary_number) {
    value = tag.number;
  } else {
    const std::string_view t = util::trim(tag.text);
    if (const auto i = util::parse_int(t)) return *i;
    const auto d = util::parse_double(t);
    value = d ? *d : 0.0;
  }
  if (!std::isfinite(value)) return 0;
  return static_cast<std::int64_t>(std::clamp(value, -9.0e18, 9.0e18));
}

std::string_view tag_string(const DxfTag& tag) noexcept { return tag.text; }

bool is_binary_dxf(ByteView data) noexcept {
  return data.size() >= binary_sentinel.size() && as_chars(data.first(binary_sentinel.size())) == binary_sentinel;
}

Result<std::vector<DxfTag>> tokenize_dxf(ByteView data, const Context& ctx) {
  if (is_binary_dxf(data)) return tokenize_binary(data, ctx);
  return tokenize_ascii(data, ctx);
}

// --------------------------------------------------------------------------- strings

void DxfStrings::configure(std::string_view acadver, std::string_view codepage) {
  const std::string_view v = util::trim(acadver);
  utf8_ = v.size() == 6 && v.starts_with("AC") && v >= "AC1021";
  charset_ = charset_of_codepage(codepage);
  guess_ = util::trim(codepage).empty() || charset_ == 0;
}

std::string DxfStrings::decode(std::string_view raw) const {
  std::string text;
  const bool ascii = std::all_of(raw.begin(), raw.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
  if (ascii || utf8_ || (guess_ && util::is_valid_utf8(raw))) {
    text = util::sanitize_utf8(raw);
  } else {
    text = metafile::charset_to_utf8(as_bytes(raw), charset_ == 0 ? 0 : charset_);
  }
  if (text.find('\\') == std::string::npos && text.find('^') == std::string::npos) return text;
  // \U+XXXX unicode escapes, \M+NXXXX multibyte escapes and ^X caret notation.
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\' && i + 6 < text.size() + 0 && (text[i + 1] == 'U' || text[i + 1] == 'u') && text[i + 2] == '+') {
      int value = 0;
      bool ok = i + 6 < text.size() + 1;
      for (std::size_t k = 0; ok && k < 4; ++k) {
        const int h = i + 3 + k < text.size() ? hex_value(text[i + 3 + k]) : -1;
        if (h < 0) ok = false;
        else value = value * 16 + h;
      }
      if (ok) {
        append_utf8(out, static_cast<char32_t>(value));
        i += 6;
        continue;
      }
    }
    if (c == '\\' && i + 7 < text.size() && (text[i + 1] == 'M' || text[i + 1] == 'm') && text[i + 2] == '+') {
      bool ok = true;
      for (std::size_t k = 4; k < 8; ++k) ok = ok && hex_value(text[i + k]) >= 0;
      if (ok) {
        append_utf8(out, util::replacement_character);
        i += 7;
        continue;
      }
    }
    if (c == '^' && i + 1 < text.size()) {
      const char n = text[i + 1];
      if (n == ' ') {
        out.push_back('^');
        ++i;
        continue;
      }
      if (n >= '@' && n <= '_') {
        out.push_back(static_cast<char>(n - '@'));
        ++i;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

// --------------------------------------------------------------------------- text codes

std::string expand_percent_codes(std::string_view text) {
  if (text.find("%%") == std::string_view::npos) return std::string(text);
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && text[i + 1] == '%') {
      const char c = util::ascii_lower(text[i + 2]);
      if (c == 'c') {
        append_utf8(out, U'Ø');
        i += 2;
        continue;
      }
      if (c == 'd') {
        append_utf8(out, U'°');
        i += 2;
        continue;
      }
      if (c == 'p') {
        append_utf8(out, U'±');
        i += 2;
        continue;
      }
      if (c == '%') {
        out.push_back('%');
        i += 2;
        continue;
      }
      if (c == 'u' || c == 'o' || c == 'k') {
        i += 2;
        continue;
      }
      if (util::is_ascii_digit(text[i + 2])) {
        std::size_t k = i + 2;
        int value = 0;
        while (k < text.size() && k < i + 5 && util::is_ascii_digit(text[k])) value = value * 10 + (text[k++] - '0');
        if (value > 0) append_utf8(out, static_cast<char32_t>(value));
        i = k - 1;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

namespace {

// The value of an inline code such as \H2.5x; up to the ';'.
std::string_view code_argument(std::string_view text, std::size_t& i) {
  const std::size_t start = i;
  while (i < text.size() && text[i] != ';') ++i;
  const std::string_view arg = text.substr(start, i - start);
  if (i < text.size()) ++i;  // skip ';'
  return arg;
}

}  // namespace

std::vector<MTextParagraph> parse_mtext(std::string_view text, double height, const MTextRun& base) {
  std::vector<MTextParagraph> paragraphs(1);
  std::vector<MTextRun> stack;
  MTextRun state = base;
  state.text.clear();
  state.height = height;
  MTextRun current = state;
  const auto flush = [&] {
    if (current.text.empty()) return;
    current.text = expand_percent_codes(current.text);
    paragraphs.back().runs.push_back(current);
    current.text.clear();
  };
  const auto set_state = [&](const MTextRun& next) {
    flush();
    state = next;
    state.text.clear();
    current = state;
  };
  std::size_t i = 0;
  int guard = 0;
  while (i < text.size()) {
    if (++guard > 10'000'000) break;
    const char c = text[i];
    if (c == '{') {
      if (stack.size() < 64) stack.push_back(state);
      ++i;
      continue;
    }
    if (c == '}') {
      if (!stack.empty()) {
        const MTextRun previous = stack.back();
        stack.pop_back();
        set_state(previous);
      }
      ++i;
      continue;
    }
    if (c == '\n') {
      flush();
      paragraphs.emplace_back();
      ++i;
      continue;
    }
    if (c == '\t') {
      current.text += "    ";
      ++i;
      continue;
    }
    if (c != '\\' || i + 1 >= text.size()) {
      current.text.push_back(c);
      ++i;
      continue;
    }
    const char code = text[i + 1];
    i += 2;
    switch (code) {
      case 'P':
      case 'X':
      case 'N':
        flush();
        paragraphs.emplace_back();
        break;
      case '~': append_utf8(current.text, U' '); break;
      case '\\':
      case '{':
      case '}': current.text.push_back(code); break;
      case 'L': { MTextRun n = state; n.underline = true; set_state(n); break; }
      case 'l': { MTextRun n = state; n.underline = false; set_state(n); break; }
      case 'O': case 'o': case 'K': case 'k': break;  // overline / strike-through: not drawn
      case 'f':
      case 'F': {
        const std::string_view arg = code_argument(text, i);
        MTextRun n = state;
        const auto parts = util::split(arg, '|');
        if (!parts.empty()) n.font = std::string(parts[0]);
        for (std::size_t k = 1; k < parts.size(); ++k) {
          if (parts[k].starts_with("b")) n.bold = parts[k] == "b1";
          if (parts[k].starts_with("i")) n.italic = parts[k] == "i1";
        }
        set_state(n);
        break;
      }
      case 'H': {
        std::string_view arg = code_argument(text, i);
        MTextRun n = state;
        const bool relative = !arg.empty() && (arg.back() == 'x' || arg.back() == 'X');
        if (relative) arg.remove_suffix(1);
        if (const auto v = util::parse_double(util::trim(arg)); v && *v > 0 && std::isfinite(*v)) {
          n.height = relative ? state.height * *v : *v;
        }
        set_state(n);
        break;
      }
      case 'C': {
        const std::string_view arg = code_argument(text, i);
        MTextRun n = state;
        if (const auto v = util::parse_int(util::trim(arg))) {
          if (*v >= 1 && *v <= 255) n.color = aci_color(static_cast<int>(*v));
          else n.color = base.color;
        }
        set_state(n);
        break;
      }
      case 'c': {
        const std::string_view arg = code_argument(text, i);
        MTextRun n = state;
        if (const auto v = util::parse_int(util::trim(arg)); v && *v >= 0) {
          const auto bgr = static_cast<std::uint32_t>(*v & 0xFFFFFF);
          n.color = Color::rgb8(static_cast<int>(bgr & 0xFF), static_cast<int>((bgr >> 8) & 0xFF),
                                static_cast<int>((bgr >> 16) & 0xFF));
        }
        set_state(n);
        break;
      }
      case 'S': {
        // Stacked fraction a^b; a/b; a#b;
        const std::string_view arg = code_argument(text, i);
        std::string stacked;
        for (std::size_t k = 0; k < arg.size(); ++k) {
          const char s = arg[k];
          if (s == '\\' && k + 1 < arg.size()) {
            stacked.push_back(arg[++k]);
          } else if (s == '#' || s == '/') {
            stacked.push_back('/');
          } else if (s == '^') {
            stacked.push_back(' ');
          } else {
            stacked.push_back(s);
          }
        }
        current.text += util::trim(stacked);
        break;
      }
      case 'A': case 'Q': case 'W': case 'T': case 'p': case 'a': case 'q': case 'w': case 't':
        (void)code_argument(text, i);
        break;
      default:
        current.text.push_back(code);
        break;
    }
  }
  flush();
  return paragraphs;
}

}  // namespace cvt::vector::cad
