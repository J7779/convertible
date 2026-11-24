// Convertible core - pim family: content lines, text escapes and calendar times.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pim/content_line.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "families/pim/pim.hpp"
#include "util/charsets.hpp"
#include "util/text.hpp"

namespace cvt::pim::cl {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/// RFC 6868 parameter value escapes: ^n, ^^, ^'.
std::string decode_caret(std::string_view value) {
  if (value.find('^') == std::string_view::npos) return std::string(value);
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '^' && i + 1 < value.size()) {
      const char next = value[i + 1];
      if (next == 'n' || next == 'N') {
        out.push_back('\n');
        ++i;
        continue;
      }
      if (next == '^') {
        out.push_back('^');
        ++i;
        continue;
      }
      if (next == '\'') {
        out.push_back('"');
        ++i;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

bool is_legacy_encoding_word(std::string_view word) {
  return util::iequals(word, "QUOTED-PRINTABLE") || util::iequals(word, "BASE64") || util::iequals(word, "8BIT") ||
         util::iequals(word, "7BIT") || util::iequals(word, "B");
}

/// Split "NAME;P=V;Q=\"a:b\"" from the value at the first ':' outside quotes.
std::size_t value_colon(std::string_view line) {
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') quoted = !quoted;
    if (line[i] == ':' && !quoted) return i;
  }
  return std::string_view::npos;
}

}  // namespace

Result<std::vector<ContentLine>> parse_lines(std::string_view text, bool legacy, const Limits& limits) {
  // Physical lines.
  std::vector<std::string_view> physical;
  {
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = text.find_first_of("\r\n", start);
      if (end == std::string_view::npos) {
        physical.push_back(text.substr(start));
        break;
      }
      physical.push_back(text.substr(start, end - start));
      start = end + ((text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ? 2 : 1);
    }
  }
  // Logical lines.
  std::vector<std::string> logical;
  for (std::size_t i = 0; i < physical.size(); ++i) {
    std::string_view line = physical[i];
    if (!logical.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      logical.back().append(line.substr(1));
      continue;
    }
    if (line.empty() || util::trim(line).empty()) continue;
    logical.emplace_back(line);
    if (logical.size() > limits.max_archive_entries * 64) {
      return fail(Status::Limit, "too many lines ({} or more)", logical.size());
    }
    // Quoted-printable soft line breaks continue on the next physical line.
    if (legacy) {
      std::string& current = logical.back();
      const std::size_t colon = value_colon(current);
      const bool qp = colon != std::string::npos &&
                      util::to_upper(std::string_view(current).substr(0, colon)).find("QUOTED-PRINTABLE") != std::string::npos;
      while (qp && !current.empty() && current.back() == '=' && i + 1 < physical.size()) {
        current.pop_back();
        ++i;
        current.append(util::trim_left(physical[i]));
      }
    }
  }

  std::vector<ContentLine> out;
  out.reserve(logical.size());
  for (const std::string& line : logical) {
    const std::size_t colon = value_colon(line);
    if (colon == std::string::npos) continue;
    const std::string_view head = std::string_view(line).substr(0, colon);
    ContentLine parsed;
    parsed.value = line.substr(colon + 1);
    // Split head on ';' outside quotes.
    std::vector<std::string_view> parts;
    {
      bool quoted = false;
      std::size_t start = 0;
      for (std::size_t i = 0; i <= head.size(); ++i) {
        if (i < head.size() && head[i] == '"') quoted = !quoted;
        if (i == head.size() || (head[i] == ';' && !quoted)) {
          parts.push_back(head.substr(start, i - start));
          start = i + 1;
        }
      }
    }
    std::string_view name = util::trim(parts[0]);
    if (const std::size_t dot = name.find('.'); dot != std::string_view::npos) {
      parsed.group = std::string(name.substr(0, dot));
      name = name.substr(dot + 1);
    }
    if (name.empty()) continue;
    parsed.name = util::to_upper(name);
    for (std::size_t p = 1; p < parts.size(); ++p) {
      const std::string_view part = util::trim(parts[p]);
      if (part.empty()) continue;
      const std::size_t eq = part.find('=');
      if (eq == std::string_view::npos) {
        // vCard 2.1 bare parameter.
        parsed.params.emplace_back(is_legacy_encoding_word(part) ? "ENCODING" : "TYPE", std::string(part));
        continue;
      }
      std::string key = util::to_upper(util::trim(part.substr(0, eq)));
      std::string_view raw = util::trim(part.substr(eq + 1));
      // Values may be a comma list of (possibly quoted) values; strip the quotes.
      std::string value;
      bool quoted = false;
      for (const char c : raw) {
        if (c == '"') {
          quoted = !quoted;
          continue;
        }
        value.push_back(c);
      }
      parsed.params.emplace_back(std::move(key), decode_caret(value));
    }
    if (legacy) {
      std::string encoding = param(parsed, "ENCODING");
      std::string charset = param(parsed, "CHARSET");
      if (util::iequals(encoding, "QUOTED-PRINTABLE")) {
        // Hard line breaks arrive as "=0D=0A"; keep them as escaped "\n" so that
        // they cannot be mistaken for structure.
        const std::string decoded = decode_charset(decode_qp(parsed.value), charset.empty() ? "utf-8" : charset);
        parsed.value = util::replace_all(util::replace_all(decoded, "\r\n", "\\n"), "\n", "\\n");
        std::erase_if(parsed.params, [](const auto& p) { return p.first == "ENCODING" || p.first == "CHARSET"; });
      } else if (!charset.empty()) {
        std::erase_if(parsed.params, [](const auto& p) { return p.first == "CHARSET"; });
      }
    }
    out.push_back(std::move(parsed));
  }
  return out;
}

std::string param(const ContentLine& line, std::string_view name) {
  for (const auto& [key, value] : line.params) {
    if (util::iequals(key, name)) return value;
  }
  return {};
}

bool has_param(const ContentLine& line, std::string_view name) {
  return std::ranges::any_of(line.params, [&](const auto& p) { return util::iequals(p.first, name); });
}

std::vector<std::string> types(const Params& params) {
  std::vector<std::string> out;
  for (const auto& [key, value] : params) {
    if (!util::iequals(key, "TYPE")) continue;
    for (const std::string_view piece : util::split(value, ',', false)) {
      out.push_back(util::to_lower(util::trim(piece)));
    }
  }
  return out;
}

std::string unescape(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      const char next = raw[i + 1];
      ++i;
      if (next == 'n' || next == 'N') out.push_back('\n');
      else out.push_back(next);
      continue;
    }
    out.push_back(raw[i]);
  }
  return out;
}

std::string escape(std::string_view text, bool semicolons, bool commas) {
  std::string out;
  out.reserve(text.size() + 8);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\\') out += "\\\\";
    else if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      out += "\\n";
    } else if (c == '\n') out += "\\n";
    else if (c == ';' && semicolons) out += "\\;";
    else if (c == ',' && commas) out += "\\,";
    else out.push_back(c);
  }
  return out;
}

std::vector<std::string> split_raw(std::string_view raw, char separator) {
  std::vector<std::string> out;
  std::string current;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      current.push_back(raw[i]);
      current.push_back(raw[i + 1]);
      ++i;
      continue;
    }
    if (raw[i] == separator) {
      out.push_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(raw[i]);
  }
  out.push_back(std::move(current));
  return out;
}

std::vector<std::string> split_unescape(std::string_view raw, char separator) {
  std::vector<std::string> out = split_raw(raw, separator);
  for (std::string& piece : out) piece = unescape(piece);
  return out;
}

std::string decode_qp(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '=' && i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string encode_qp(std::string_view text, std::size_t first_line_used, bool soft_breaks) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string out;
  std::size_t column = first_line_used;
  const auto emit = [&](std::string_view piece) {
    if (soft_breaks && column + piece.size() > 75) {
      out += "=\r\n";
      column = 0;
    }
    out.append(piece);
    column += piece.size();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte == '\n') {
      emit("=0D=0A");
      continue;
    }
    if (byte == '\r') continue;
    const bool last = i + 1 == text.size();
    if ((byte >= 33 && byte <= 126 && byte != '=') || (byte == ' ' && !last)) {
      const char c = static_cast<char>(byte);
      emit(std::string_view(&c, 1));
    } else {
      const char encoded[3] = {'=', digits[byte >> 4], digits[byte & 15]};
      emit(std::string_view(encoded, 3));
    }
  }
  return out;
}

std::string decode_charset(std::string_view bytes, std::string_view charset) {
  if (charset.empty() || util::iequals(charset, "us-ascii") || util::iequals(charset, "ascii")) {
    return util::sanitize_utf8(bytes);
  }
  if (const auto encoding = util::parse_encoding(charset)) {
    std::string text = util::decode_text(as_bytes(bytes), *encoding).text;
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
  }
  // Legacy charsets (CHARSET=SHIFT_JIS on Japanese phones, BIG5, KOI8-R ...).
  if (auto decoded = util::decode_labelled(as_bytes(bytes), charset)) return std::move(decoded->text);
  return util::sanitize_utf8(bytes);
}

std::string param_value(std::string_view value) {
  std::string clean;
  for (const char c : value) {
    if (c == '"') clean += "^'";
    else if (c == '\n') clean += "^n";
    else if (c != '\r') clean.push_back(c);
  }
  if (clean.find_first_of(":;,") != std::string::npos) return "\"" + clean + "\"";
  return clean;
}

std::string fold(std::string_view line) {
  std::string out;
  std::size_t limit = 75;
  while (line.size() > limit) {
    std::size_t cut = limit;
    while (cut > 1 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
    out.append(line.substr(0, cut));
    out += "\r\n ";
    line.remove_prefix(cut);
    limit = 74;
  }
  out.append(line);
  out += "\r\n";
  return out;
}

std::string format_line(std::string_view name, const Params& params, std::string_view value) {
  std::string line(name);
  for (const auto& [key, v] : params) {
    line.push_back(';');
    line.append(key);
    line.push_back('=');
    line.append(param_value(v));
  }
  line.push_back(':');
  line.append(value);
  return fold(line);
}

// --------------------------------------------------------------------------- times

std::optional<ir::CalendarTime> parse_time(std::string_view value, std::string_view tzid) {
  const auto parsed = parse_iso_datetime(util::trim(value));
  if (!parsed) return std::nullopt;
  ir::CalendarTime time;
  std::int64_t seconds = parsed->local_seconds;
  if (parsed->offset_minutes) {
    seconds = parsed->utc_seconds();
    time.utc = true;
  } else if (!parsed->date_only) {
    time.tzid = std::string(tzid);
  }
  time.date_only = parsed->date_only;
  ir::CalendarTime result = time_from_seconds(seconds, time);
  if (result.year < 0 || result.year > 9999) return std::nullopt;
  return result;
}

std::string format_time(const ir::CalendarTime& time) {
  if (time.date_only) return std::format("{:04}{:02}{:02}", time.year, time.month, time.day);
  return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}{}", time.year, time.month, time.day, time.hour, time.minute,
                     time.second, time.utc ? "Z" : "");
}

std::string iso_time(const ir::CalendarTime& time) {
  if (time.date_only) return std::format("{:04}-{:02}-{:02}", time.year, time.month, time.day);
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}{}", time.year, time.month, time.day, time.hour, time.minute,
                     time.second, time.utc ? "Z" : "");
}

std::int64_t civil_seconds(const ir::CalendarTime& time) {
  const unsigned month = static_cast<unsigned>(std::clamp(time.month, 1, 12));
  const unsigned day = static_cast<unsigned>(std::clamp(time.day, 1, 31));
  return days_from_civil(time.year, month, day) * 86400 + (time.date_only ? 0 : time.hour * 3600 + time.minute * 60 + time.second);
}

ir::CalendarTime time_from_seconds(std::int64_t seconds, const ir::CalendarTime& like) {
  std::int64_t days = seconds / 86400;
  std::int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  ir::CalendarTime out = like;
  out.year = static_cast<int>(std::clamp<std::int64_t>(date.year, -100000, 100000));
  out.month = static_cast<int>(date.month);
  out.day = static_cast<int>(date.day);
  if (like.date_only) {
    out.hour = out.minute = out.second = 0;
  } else {
    out.hour = static_cast<int>(rest / 3600);
    out.minute = static_cast<int>((rest / 60) % 60);
    out.second = static_cast<int>(rest % 60);
  }
  return out;
}

std::optional<std::int64_t> parse_duration(std::string_view value) {
  value = util::trim(value);
  int sign = 1;
  if (!value.empty() && (value[0] == '+' || value[0] == '-')) {
    sign = value[0] == '-' ? -1 : 1;
    value.remove_prefix(1);
  }
  if (value.empty() || (value[0] != 'P' && value[0] != 'p')) return std::nullopt;
  value.remove_prefix(1);
  std::int64_t total = 0;
  bool in_time = false;
  bool any = false;
  std::int64_t number = -1;
  for (const char c : value) {
    if (util::is_ascii_digit(c)) {
      if (number < 0) number = 0;
      if (number > 100'000'000) return std::nullopt;
      number = number * 10 + (c - '0');
      continue;
    }
    const char u = util::ascii_upper(c);
    if (u == 'T') {
      in_time = true;
      continue;
    }
    if (number < 0) return std::nullopt;
    std::int64_t unit = 0;
    if (u == 'W' && !in_time) unit = 7 * 86400;
    else if (u == 'D' && !in_time) unit = 86400;
    else if (u == 'H' && in_time) unit = 3600;
    else if (u == 'M' && in_time) unit = 60;
    else if (u == 'S' && in_time) unit = 1;
    else return std::nullopt;
    total += number * unit;
    number = -1;
    any = true;
  }
  if (!any || number >= 0) return std::nullopt;
  return sign * total;
}

std::string format_duration(std::int64_t seconds) {
  std::string out = seconds < 0 ? "-P" : "P";
  std::uint64_t magnitude = seconds < 0 ? static_cast<std::uint64_t>(-seconds) : static_cast<std::uint64_t>(seconds);
  if (magnitude == 0) return "PT0S";
  if (magnitude % (7 * 86400) == 0) return out + std::format("{}W", magnitude / (7 * 86400));
  const std::uint64_t days = magnitude / 86400;
  magnitude %= 86400;
  if (days > 0) out += std::format("{}D", days);
  if (magnitude > 0) {
    out.push_back('T');
    if (magnitude / 3600 > 0) out += std::format("{}H", magnitude / 3600);
    if ((magnitude / 60) % 60 > 0) out += std::format("{}M", (magnitude / 60) % 60);
    if (magnitude % 60 > 0) out += std::format("{}S", magnitude % 60);
  }
  return out;
}

}  // namespace cvt::pim::cl
