// Convertible core - data family: YAML 1.2 (and the YAML 1.1 schema).
//
// Reader: block and flow collections, all five scalar styles (plain, single
// and double quoted, literal and folded block scalars with indentation and
// chomping indicators), anchors and aliases (with a node budget against
// "billion laughs" documents; recursive aliases are an error), merge keys,
// %YAML / %TAG directives, explicit and verbatim tags for the core types plus
// !!binary, !!timestamp, !!omap, !!pairs and !!set, and multi-document
// streams (see the `documents` option). Untagged plain scalars are resolved
// with the YAML 1.2 core schema, or the 1.1 schema (yes/no, 0b, 012 octal,
// 1_000, sexagesimal) when the stream says %YAML 1.1 or `yaml_version` asks.
// Timestamps are recognised unless `timestamps` is off. Comments are ignored.
// Keys that are not strings become their source text (scalars) or compact
// JSON (collections).
//
// Writer: block style with idiomatic quoting - plain where every YAML 1.1 and
// 1.2 reader resolves the text back to the same string, single quotes where
// that is enough, double quotes with escapes for control characters, and
// literal block scalars for multi-line text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

constexpr std::string_view yaml_core = "tag:yaml.org,2002:";

// =========================================================================== scalar resolution

bool all_of_set(std::string_view text, std::string_view allowed) {
  return std::ranges::all_of(text, [&](char c) { return allowed.find(c) != std::string_view::npos; });
}

bool is_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return util::is_ascii_digit(c); });
}

/// YAML 1.2 core float: [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?
bool core_float(std::string_view text) {
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
  const std::size_t int_start = i;
  while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
  const std::size_t int_digits = i - int_start;
  std::size_t frac_digits = 0;
  bool dot = false;
  if (i < text.size() && text[i] == '.') {
    dot = true;
    ++i;
    const std::size_t frac_start = i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    frac_digits = i - frac_start;
  }
  if (int_digits == 0 && frac_digits == 0) return false;
  (void)dot;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
    if (!is_digits(text.substr(i))) return false;
    i = text.size();
  }
  return i == text.size();
}

std::optional<Json> special_float(std::string_view text) {
  std::string_view body = text;
  bool negative = false;
  if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  if (body == ".inf" || body == ".Inf" || body == ".INF") {
    return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
  }
  if (body.size() == text.size() && (text == ".nan" || text == ".NaN" || text == ".NAN")) {
    return Json(std::numeric_limits<double>::quiet_NaN());
  }
  return std::nullopt;
}

/// "2001-12-14", "2001-12-14t21:59:43.10-05:00", "2001-12-14 21:59:43.10 -5".
std::optional<std::string> yaml_timestamp(std::string_view text) {
  if (text.size() < 8 || !util::is_ascii_digit(text[0])) return std::nullopt;
  std::size_t i = 0;
  const auto number = [&](std::size_t min_digits, std::size_t max_digits, int& out) {
    const std::size_t start = i;
    int value = 0;
    while (i < text.size() && util::is_ascii_digit(text[i]) && i - start < max_digits) value = value * 10 + (text[i++] - '0');
    out = value;
    return i - start >= min_digits;
  };
  int year = 0, month = 0, day = 0;
  if (!number(4, 4, year) || i >= text.size() || text[i++] != '-') return std::nullopt;
  const std::size_t month_start = i;
  if (!number(1, 2, month) || i >= text.size() || text[i++] != '-') return std::nullopt;
  const bool short_form = i - month_start == 3;
  const std::size_t day_start = i;
  if (!number(1, 2, day)) return std::nullopt;
  DateTime value;
  value.year = year;
  value.month = month;
  value.day = day;
  if (i == text.size()) {
    if (!short_form || i - day_start != 2) return std::nullopt;  // date-only form needs 2-digit fields
    value.kind = DateTime::Kind::Date;
    auto checked = parse_datetime(format_datetime(value));
    if (!checked) return std::nullopt;
    return format_datetime(*checked);
  }
  if (text[i] == 'T' || text[i] == 't') {
    ++i;
  } else if (text[i] == ' ' || text[i] == '\t') {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  } else {
    return std::nullopt;
  }
  if (!number(1, 2, value.hour) || i >= text.size() || text[i++] != ':' || !number(2, 2, value.minute) ||
      i >= text.size() || text[i++] != ':' || !number(2, 2, value.second)) {
    return std::nullopt;
  }
  if (i < text.size() && text[i] == '.') {
    const std::size_t start = ++i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    value.fraction = std::string(text.substr(start, i - start));
  }
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  if (i == text.size()) {
    value.kind = DateTime::Kind::Local;
  } else if (text[i] == 'Z' && i + 1 == text.size()) {
    value.kind = DateTime::Kind::Offset;
    value.zulu = true;
  } else if (text[i] == '+' || text[i] == '-') {
    const int sign = text[i++] == '-' ? -1 : 1;
    int hours = 0;
    int minutes = 0;
    if (!number(1, 2, hours)) return std::nullopt;
    if (i < text.size() && text[i] == ':') {
      ++i;
      if (!number(2, 2, minutes)) return std::nullopt;
    }
    if (i != text.size()) return std::nullopt;
    value.kind = DateTime::Kind::Offset;
    value.offset_minutes = sign * (hours * 60 + minutes);
    value.zulu = value.offset_minutes == 0;
  } else {
    return std::nullopt;
  }
  if (value.month < 1 || value.month > 12 || value.hour > 23 || value.minute > 59 || value.second > 60 ||
      std::abs(value.offset_minutes) >= 24 * 60) {
    return std::nullopt;
  }
  const std::string canonical = format_datetime(value);
  if (!parse_datetime(canonical)) return std::nullopt;  // day of month out of range
  return canonical;
}

struct Schema {
  bool yaml11 = false;
  bool timestamps = true;
};

std::optional<Json> resolve_int_11(std::string_view text) {
  std::string_view body = text;
  bool negative = false;
  if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  if (body.empty()) return std::nullopt;
  std::string digits;
  const auto strip = [&](std::string_view part) {
    digits.clear();
    for (const char c : part) {
      if (c != '_') digits.push_back(c);
    }
    return !digits.empty();
  };
  if (body.starts_with("0b")) {
    if (!all_of_set(body.substr(2), "01_") || !strip(body.substr(2))) return std::nullopt;
    return integer_from_digits(digits, 2, negative);
  }
  if (body.starts_with("0x")) {
    if (!all_of_set(body.substr(2), "0123456789abcdefABCDEF_") || !strip(body.substr(2))) return std::nullopt;
    return integer_from_digits(digits, 16, negative);
  }
  if (body.size() > 1 && body.front() == '0') {
    if (!all_of_set(body.substr(1), "01234567_") || !strip(body.substr(1))) return std::nullopt;
    return integer_from_digits(digits, 8, negative);
  }
  if (body.find(':') != std::string_view::npos) {  // sexagesimal 190:20:30
    if (body.front() == '0') return std::nullopt;
    std::int64_t total = 0;
    for (const std::string_view part : util::split(body, ':')) {
      if (part.empty() || !all_of_set(part, "0123456789_") || !strip(part)) return std::nullopt;
      const auto value = util::parse_int(digits);
      if (!value || total > std::numeric_limits<std::int64_t>::max() / 60 - *value) return std::nullopt;
      total = total * 60 + *value;
    }
    return Json(negative ? -total : total);
  }
  if (!all_of_set(body, "0123456789_") || body.front() == '_' || !strip(body)) return std::nullopt;
  return number_from_literal((negative ? "-" : "") + digits);
}

std::optional<Json> resolve_float_11(std::string_view text) {
  if (auto special = special_float(text)) return special;
  std::string_view body = text;
  std::string sign;
  if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
    if (body.front() == '-') sign = "-";
    body.remove_prefix(1);
  }
  if (body.find(':') != std::string_view::npos) {  // 190:20:30.15
    const std::size_t last = body.rfind(':');
    std::string_view head = body.substr(0, last);
    const std::string_view tail = body.substr(last + 1);
    auto whole = resolve_int_11(head);
    std::string tail_digits;
    for (const char c : tail) {
      if (c != '_') tail_digits.push_back(c);
    }
    if (!whole || !core_float(tail_digits)) return std::nullopt;
    const double value = whole->as_double() * 60 + util::parse_double(tail_digits).value_or(0.0);
    return Json(sign == "-" ? -value : value);
  }
  // [0-9][0-9_]*?\.[0-9_]*([eE][-+][0-9]+)?  (a dot is required)
  if (body.find('.') == std::string_view::npos) return std::nullopt;
  std::string cleaned = sign;
  for (const char c : body) {
    if (c != '_') cleaned.push_back(c);
  }
  if (!core_float(cleaned)) return std::nullopt;
  return number_from_literal(cleaned);
}

Json resolve_plain(std::string_view text, const Schema& schema) {
  if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL") return Json();
  if (text == "true" || text == "True" || text == "TRUE") return Json(true);
  if (text == "false" || text == "False" || text == "FALSE") return Json(false);
  const char first = text.front();
  const bool numeric_start = util::is_ascii_digit(first) || first == '-' || first == '+' || first == '.';
  if (schema.yaml11) {
    if (text == "yes" || text == "Yes" || text == "YES" || text == "on" || text == "On" || text == "ON") {
      return Json(true);
    }
    if (text == "no" || text == "No" || text == "NO" || text == "off" || text == "Off" || text == "OFF") {
      return Json(false);
    }
    if (numeric_start) {
      if (auto value = resolve_int_11(text)) return std::move(*value);
      if (auto value = resolve_float_11(text)) return std::move(*value);
    }
  } else if (numeric_start) {
    std::string_view body = text;
    bool negative = false;
    if (body.front() == '-' || body.front() == '+') {
      negative = body.front() == '-';
      body.remove_prefix(1);
    }
    if (is_digits(body)) return number_from_literal(text);
    if (body.size() > 2 && body.starts_with("0o") && text.size() == body.size() && all_of_set(body.substr(2), "01234567")) {
      if (auto value = integer_from_digits(body.substr(2), 8, negative)) return std::move(*value);
    }
    if (body.size() > 2 && body.starts_with("0x") && text.size() == body.size() &&
        all_of_set(body.substr(2), "0123456789abcdefABCDEF")) {
      if (auto value = integer_from_digits(body.substr(2), 16, negative)) return std::move(*value);
    }
    if (core_float(text)) return number_from_literal(text);
    if (auto special = special_float(text)) return std::move(*special);
  }
  if (schema.timestamps && util::is_ascii_digit(first)) {
    if (auto stamp = yaml_timestamp(text)) return make_date(std::move(*stamp));
  }
  return Json(std::string(text));
}

// =========================================================================== parser

struct Properties {
  std::string anchor;
  std::string tag;  // resolved: "tag:yaml.org,2002:str", "!local", "!" (non-specific)
  std::size_t tag_at = 0;
  [[nodiscard]] bool empty() const noexcept { return anchor.empty() && tag.empty(); }
};

class YamlParser {
 public:
  YamlParser(std::string_view text, ReadContext& ctx, Schema schema, bool forced_version, bool merge_keys)
      : t_(text), ctx_(ctx), schema_(schema), forced_version_(forced_version), merge_keys_(merge_keys),
        policy_(read_policy(ctx.options)) {
    alias_budget_ = std::max<std::uint64_t>(200'000, static_cast<std::uint64_t>(text.size()) * 64);
  }

  Result<std::vector<Json>> parse_stream() {
    std::vector<Json> documents;
    CVT_TRY(check_characters());
    while (true) {
      skip_space_lines();
      if (at_end()) break;
      // Directives.
      bool directives = false;
      handles_.clear();
      Schema document_schema = schema_;
      while (!at_end() && column() == 0 && peek() == '%') {
        CVT_TRY(parse_directive(document_schema));
        directives = true;
        skip_space_lines();
      }
      active_schema_ = document_schema;
      bool explicit_start = false;
      if (at_marker("---")) {
        p_ += 3;
        explicit_start = true;
      } else if (directives) {
        return error("expected '---' after the directives");
      } else if (at_marker("...")) {
        p_ += 3;
        continue;
      }
      anchors_.clear();
      CVT_TRY_ASSIGN(Json document, parse_block_node(-1, 0, false));
      documents.push_back(std::move(document));
      if (documents.size() > 1'000'000) return fail(Status::Limit, "the YAML stream has too many documents");
      skip_space_lines();
      if (at_marker("...")) {
        p_ += 3;
        skip_blanks_and_comment();
        if (!at_end() && peek() != '\n') return error("unexpected content after '...'");
        continue;
      }
      if (at_end()) break;
      if (at_marker("---")) continue;
      (void)explicit_start;
      return error(column() == 0 ? "unexpected content; expected the end of the document"
                                 : "bad indentation of a mapping entry or sequence item");
    }
    if (unknown_tags_ > 0) {
      ctx_.warn(std::format("{} node(s) carried application-specific YAML tags, which were ignored.", unknown_tags_));
    }
    return documents;
  }

 private:
  // ------------------------------------------------------------------ position helpers

  [[nodiscard]] bool at_end() const noexcept { return p_ >= t_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
    return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0';
  }
  [[nodiscard]] static bool blank(char c) noexcept { return c == ' ' || c == '\t'; }
  [[nodiscard]] bool blank_or_end(std::size_t at) const noexcept {
    return at >= t_.size() || t_[at] == ' ' || t_[at] == '\t' || t_[at] == '\n';
  }
  [[nodiscard]] static bool flow_indicator(char c) noexcept {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
  }
  /// Start of the line containing `at`. Parsing moves forward, so the last
  /// answer is cached and only the bytes since then are searched.
  [[nodiscard]] std::size_t line_start(std::size_t at) const noexcept {
    std::size_t result = 0;
    if (at >= cache_at_) {
      const std::string_view window = t_.substr(cache_at_, at - cache_at_);
      const std::size_t nl = window.rfind('\n');
      result = nl == std::string_view::npos ? cache_start_ : cache_at_ + nl + 1;
    } else {
      const std::size_t nl = at == 0 ? std::string_view::npos : t_.rfind('\n', at - 1);
      return nl == std::string_view::npos ? 0 : nl + 1;
    }
    cache_at_ = at;
    cache_start_ = result;
    return result;
  }
  [[nodiscard]] int column() const noexcept { return static_cast<int>(p_ - line_start(p_)); }
  [[nodiscard]] bool at_marker(std::string_view marker) const noexcept {
    return column() == 0 && t_.substr(p_, 3) == marker && blank_or_end(p_ + 3);
  }
  [[nodiscard]] bool at_marker_at(std::size_t at) const noexcept {
    return line_start(at) == at && (t_.substr(at, 3) == "---" || t_.substr(at, 3) == "...") && blank_or_end(at + 3);
  }

  std::unexpected<Error> error(std::string_view message) const { return syntax_error("YAML", t_, p_, message); }
  std::unexpected<Error> error_at(std::size_t at, std::string_view message) const {
    return syntax_error("YAML", t_, at, message);
  }

  Result<void> check_characters() const {
    for (std::size_t i = 0; i < t_.size(); ++i) {
      const auto c = static_cast<unsigned char>(t_[i]);
      if (c < 0x20 && c != '\t' && c != '\n') {
        return error_at(i, std::format("control character U+{:04X} is not allowed in YAML", static_cast<unsigned>(c)));
      }
    }
    return {};
  }

  void skip_blanks() {
    while (blank(peek())) ++p_;
  }
  /// Blanks and a trailing comment on the current line (not the break).
  void skip_blanks_and_comment() {
    skip_blanks();
    if (peek() == '#') {
      while (!at_end() && peek() != '\n') ++p_;
    }
  }
  /// Blanks, comments and line breaks. Returns true when a line break was crossed.
  bool skip_space_lines() {
    bool crossed = false;
    while (!at_end()) {
      const char c = peek();
      if (blank(c)) {
        ++p_;
      } else if (c == '\n') {
        ++p_;
        crossed = true;
      } else if (c == '#' && (p_ == 0 || blank(t_[p_ - 1]) || t_[p_ - 1] == '\n')) {
        while (!at_end() && peek() != '\n') ++p_;
      } else {
        break;
      }
    }
    return crossed;
  }
  /// After crossing to a new line in block context, the indentation must be spaces.
  Result<void> check_indentation_tabs() const {
    for (std::size_t i = line_start(p_); i < p_; ++i) {
      if (t_[i] == '\t') return error_at(i, "tab characters cannot be used for indentation");
    }
    return {};
  }

  // ------------------------------------------------------------------ directives and properties

  Result<void> parse_directive(Schema& schema) {
    const std::size_t start = p_;
    std::size_t end = t_.find('\n', p_);
    if (end == std::string_view::npos) end = t_.size();
    std::string_view line = t_.substr(p_ + 1, end - p_ - 1);
    if (const std::size_t hash = line.find(" #"); hash != std::string_view::npos) line = line.substr(0, hash);
    std::vector<std::string_view> parts;
    for (const std::string_view part : util::split(line, ' ', false)) {
      if (!util::trim(part).empty()) parts.push_back(util::trim(part));
    }
    p_ = end;
    if (parts.empty()) return error_at(start, "empty directive");
    if (parts[0] == "YAML") {
      if (parts.size() < 2) return error_at(start, "the %YAML directive needs a version");
      if (!forced_version_) schema.yaml11 = parts[1] == "1.1" || parts[1] == "1.0";
      if (!parts[1].starts_with("1.")) return error_at(start, "unsupported YAML version");
      return {};
    }
    if (parts[0] == "TAG") {
      if (parts.size() < 3) return error_at(start, "the %TAG directive needs a handle and a prefix");
      handles_[std::string(parts[1])] = std::string(parts[2]);
      return {};
    }
    return {};  // reserved directives are ignored
  }

  [[nodiscard]] std::string percent_decode(std::string_view text) const {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size()) {
        if (const auto value = util::parse_int(text.substr(i + 1, 2), 16)) {
          out.push_back(static_cast<char>(*value));
          i += 2;
          continue;
        }
      }
      out.push_back(text[i]);
    }
    return out;
  }

  Result<std::string> parse_tag() {
    const std::size_t start = p_;
    ++p_;  // '!'
    if (peek() == '<') {
      const std::size_t close = t_.find('>', p_);
      if (close == std::string_view::npos || t_.substr(p_, close - p_).find('\n') != std::string_view::npos) {
        return error_at(start, "unterminated verbatim tag");
      }
      std::string tag = percent_decode(t_.substr(p_ + 1, close - p_ - 1));
      p_ = close + 1;
      return tag;
    }
    const std::size_t name_start = p_;
    while (!at_end() && !blank_or_end(p_) && !flow_indicator(peek())) ++p_;
    const std::string_view body = t_.substr(name_start, p_ - name_start);
    // Handles: "!" (primary), "!!" (secondary), "!name!".
    std::string handle = "!";
    std::string_view suffix = body;
    if (body.starts_with('!')) {
      handle = "!!";
      suffix = body.substr(1);
    } else if (const std::size_t bang = body.find('!'); bang != std::string_view::npos) {
      handle = "!" + std::string(body.substr(0, bang + 1));
      suffix = body.substr(bang + 1);
    }
    if (body.empty()) return std::string("!");
    if (const auto found = handles_.find(handle); found != handles_.end()) {
      return found->second + percent_decode(suffix);
    }
    if (handle == "!!") return std::string(yaml_core) + percent_decode(suffix);
    if (handle == "!") return "!" + percent_decode(suffix);
    return error_at(start, std::format("undefined tag handle '{}'", handle));
  }

  /// `&anchor` and `!tag` in any order. Leaves p_ after them (and blanks).
  Result<Properties> parse_properties() {
    Properties props;
    while (true) {
      if (peek() == '&' && props.anchor.empty()) {
        const std::size_t start = ++p_;
        while (!at_end() && !blank_or_end(p_) && !flow_indicator(peek())) ++p_;
        if (p_ == start) return error("expected an anchor name after '&'");
        props.anchor = std::string(t_.substr(start, p_ - start));
      } else if (peek() == '!' && props.tag.empty()) {
        props.tag_at = p_;
        CVT_TRY_ASSIGN(props.tag, parse_tag());
      } else {
        break;
      }
      skip_blanks();
    }
    return props;
  }

  // ------------------------------------------------------------------ anchors

  static std::uint64_t node_count(const Json& value) {
    std::uint64_t count = 0;
    std::vector<const Json*> stack{&value};
    while (!stack.empty()) {
      const Json* node = stack.back();
      stack.pop_back();
      ++count;
      for (const Json& element : node->elements()) stack.push_back(&element);
      for (const auto& member : node->items()) stack.push_back(&member.value());
    }
    return count;
  }

  void define_anchor(const std::string& name, const Json& value) {
    anchors_[name] = Anchor{value, node_count(value)};
  }

  Result<Json> parse_alias() {
    const std::size_t start = p_;
    ++p_;  // '*'
    const std::size_t name_start = p_;
    while (!at_end() && !blank_or_end(p_) && !flow_indicator(peek())) ++p_;
    const std::string name(t_.substr(name_start, p_ - name_start));
    if (name.empty()) return error_at(start, "expected an alias name after '*'");
    const auto found = anchors_.find(name);
    if (found == anchors_.end()) {
      if (std::ranges::find(open_anchors_, name) != open_anchors_.end()) {
        return error_at(start, std::format("alias *{} refers to a node that contains it (a recursive structure "
                                           "cannot be represented)", name));
      }
      return error_at(start, std::format("undefined alias *{}", name));
    }
    if (found->second.nodes > alias_budget_ - std::min(alias_budget_, alias_nodes_)) {
      return fail(Status::Limit, "the YAML aliases expand to more than {} nodes (a \"billion laughs\" document?)",
                  alias_budget_);
    }
    alias_nodes_ += found->second.nodes;
    return found->second.value;
  }

  // ------------------------------------------------------------------ tags

  Result<Json> apply_scalar_tag(const Properties& props, std::string text, bool plain, std::size_t at) {
    const std::string& tag = props.tag;
    if (tag.empty()) return plain ? resolve_plain(text, active_schema_) : Json(std::move(text));
    if (tag == "!") return Json(std::move(text));
    if (!tag.starts_with(yaml_core)) {
      ++unknown_tags_;
      return plain ? resolve_plain(text, active_schema_) : Json(std::move(text));
    }
    const std::string_view type = std::string_view(tag).substr(yaml_core.size());
    const auto bad = [&](std::string_view what) {
      return error_at(at, std::format("'{}' is not a valid {}", text.size() > 40 ? text.substr(0, 40) + "..." : text,
                                      what));
    };
    if (type == "str") return Json(std::move(text));
    if (type == "null") {
      if (!text.empty() && text != "~" && text != "null" && text != "Null" && text != "NULL") return bad("!!null");
      return Json();
    }
    if (type == "bool") {
      Schema both{true, false};
      const Json value = resolve_plain(text, both);
      if (!value.is_bool()) return bad("!!bool");
      return value;
    }
    if (type == "int") {
      Json value = resolve_plain(text, active_schema_);
      if (!value.is_integer() && tag_kind(value) != TagKind::BigInt) {
        if (auto other = resolve_int_11(text)) value = std::move(*other);
      }
      if (!value.is_integer() && tag_kind(value) != TagKind::BigInt) return bad("!!int");
      return value;
    }
    if (type == "float") {
      Json value = resolve_plain(text, active_schema_);
      if (!value.is_number() && tag_kind(value) != TagKind::Decimal && tag_kind(value) != TagKind::BigInt) {
        if (auto other = resolve_float_11(text)) value = std::move(*other);
      }
      if (value.is_integer()) return Json(value.as_double());
      if (tag_kind(value) == TagKind::BigInt) return make_decimal(std::string(tag_text(value)) + ".0");
      if (!value.is_number() && tag_kind(value) != TagKind::Decimal) return bad("!!float");
      return value;
    }
    if (type == "binary") {
      auto bytes = util::base64_decode(text);
      if (!bytes) return bad("!!binary (Base64)");
      return make_binary(*bytes);
    }
    if (type == "timestamp") {
      auto stamp = yaml_timestamp(util::trim(text));
      if (!stamp) return bad("!!timestamp");
      return make_date(std::move(*stamp));
    }
    if (type == "map" || type == "seq" || type == "omap" || type == "pairs" || type == "set") {
      return error_at(at, std::format("a scalar cannot have the tag !!{}", type));
    }
    ++unknown_tags_;
    return plain ? resolve_plain(text, active_schema_) : Json(std::move(text));
  }

  Result<Json> apply_collection_tag(const Properties& props, Json value) {
    const std::string& tag = props.tag;
    if (tag.empty() || tag == "!") return value;
    if (!tag.starts_with(yaml_core)) {
      ++unknown_tags_;
      return value;
    }
    const std::string_view type = std::string_view(tag).substr(yaml_core.size());
    if (type == "map" || type == "set") {
      if (!value.is_object()) return error_at(props.tag_at, std::format("!!{} needs a mapping", type));
      return value;
    }
    if (type == "seq" || type == "pairs") {
      if (!value.is_array()) return error_at(props.tag_at, std::format("!!{} needs a sequence", type));
      return value;
    }
    if (type == "omap") {
      if (!value.is_array()) return error_at(props.tag_at, "!!omap needs a sequence of single-pair mappings");
      Json object = Json::object();
      for (Json& pair : value.elements()) {
        if (!pair.is_object() || pair.size() != 1) {
          return error_at(props.tag_at, "!!omap needs a sequence of single-pair mappings");
        }
        auto& member = pair.items()[0];
        CVT_TRY(put_member(object, member.key(), std::move(member.value()), policy_));
      }
      return object;
    }
    return error_at(props.tag_at, std::format("a collection cannot have the tag !!{}", type));
  }

  Result<Json> finish_node(const Properties& props, Json value) {
    if (!props.anchor.empty()) define_anchor(props.anchor, value);
    return value;
  }

  // ------------------------------------------------------------------ block nodes

  /// A node in block context whose content must be indented more than
  /// `parent` when it starts on a later line. `compact_seq`: a block sequence
  /// may start at exactly `parent` (a mapping value).
  Result<Json> parse_block_node(int parent, std::size_t depth, bool compact_seq) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the YAML data is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    if (depth % 64 == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(p_) / static_cast<double>(t_.size() + 1)));
    bool crossed = skip_space_lines();
    if (at_end() || at_marker("---") || at_marker("...")) return Json();
    if (crossed) {
      CVT_TRY(check_indentation_tabs());
      const int col = column();
      if (col < parent || (col == parent && !(compact_seq && is_sequence_entry()))) return Json();
    }
    Properties props;
    std::size_t props_line = line_start(p_);
    if (peek() == '&' || peek() == '!') {
      CVT_TRY_ASSIGN(props, parse_properties());
      if (peek() == '#' || peek() == '\n' || at_end()) {
        skip_blanks_and_comment();
        crossed = skip_space_lines();
        const int col = column();
        if (at_end() || at_marker("---") || at_marker("...") ||
            (crossed && (col < parent || (col == parent && !(compact_seq && is_sequence_entry()))))) {
          CVT_TRY_ASSIGN(Json empty, apply_scalar_tag(props, "", true, props.tag_at));
          return finish_node(props, std::move(empty));
        }
        if (crossed) CVT_TRY(check_indentation_tabs());
        props_line = std::string_view::npos;  // the content is on its own line
      }
    }
    const int col = column();
    const char c = peek();
    // Properties on the same line as an implicit key belong to that key.
    if (c != '-' || !blank_or_end(p_ + 1)) {
      if (c != '?' || !blank_or_end(p_ + 1)) {
        if (c != '|' && c != '>' && implicit_key_ahead(p_)) {
          const bool props_for_key = props_line == line_start(p_) && !props.empty();
          Properties key_props = props_for_key ? props : Properties{};
          Properties map_props = props_for_key ? Properties{} : props;
          CVT_TRY_ASSIGN(Json map, parse_block_mapping(col, depth, std::move(key_props)));
          CVT_TRY_ASSIGN(map, apply_collection_tag(map_props, std::move(map)));
          return finish_node(map_props, std::move(map));
        }
      } else {
        CVT_TRY_ASSIGN(Json map, parse_block_mapping(col, depth, Properties{}));
        CVT_TRY_ASSIGN(map, apply_collection_tag(props, std::move(map)));
        return finish_node(props, std::move(map));
      }
    } else {
      if (!props.anchor.empty()) open_anchors_.push_back(props.anchor);
      auto seq = parse_block_sequence(col, depth);
      if (!props.anchor.empty()) open_anchors_.pop_back();
      if (!seq) return std::unexpected(std::move(seq).error());
      CVT_TRY_ASSIGN(Json value, apply_collection_tag(props, std::move(*seq)));
      return finish_node(props, std::move(value));
    }
    if (c == '*') {
      if (!props.empty()) return error("an alias cannot have properties");
      CVT_TRY_ASSIGN(Json value, parse_alias());
      CVT_TRY(expect_line_end());
      return value;
    }
    if (c == '|' || c == '>') {
      const std::size_t at = p_;
      CVT_TRY_ASSIGN(std::string text, parse_block_scalar(parent));
      CVT_TRY_ASSIGN(Json value, apply_scalar_tag(props, std::move(text), false, at));
      return finish_node(props, std::move(value));
    }
    if (c == '[' || c == '{') {
      if (!props.anchor.empty()) open_anchors_.push_back(props.anchor);
      auto flow = parse_flow_collection(depth + 1);
      if (!props.anchor.empty()) open_anchors_.pop_back();
      if (!flow) return std::unexpected(std::move(flow).error());
      CVT_TRY(expect_line_end());
      CVT_TRY_ASSIGN(Json value, apply_collection_tag(props, std::move(*flow)));
      return finish_node(props, std::move(value));
    }
    if (c == '"' || c == '\'') {
      const std::size_t at = p_;
      CVT_TRY_ASSIGN(std::string text, c == '"' ? parse_double_quoted() : parse_single_quoted());
      CVT_TRY(expect_line_end());
      CVT_TRY_ASSIGN(Json value, apply_scalar_tag(props, std::move(text), false, at));
      return finish_node(props, std::move(value));
    }
    if (c == '}' || c == ']' || c == ',') return error(std::format("unexpected '{}'", c));
    if (c == '@' || c == '`') return error(std::format("'{}' is reserved and cannot start a plain scalar", c));
    if (c == '%' && col == 0) return error("unexpected directive inside a document");
    const std::size_t at = p_;
    CVT_TRY_ASSIGN(std::string text, parse_plain(parent, false));
    if (text.empty() && props.empty()) return error("expected a value");
    CVT_TRY_ASSIGN(Json value, apply_scalar_tag(props, std::move(text), true, at));
    return finish_node(props, std::move(value));
  }

  [[nodiscard]] bool is_sequence_entry() const noexcept { return peek() == '-' && blank_or_end(p_ + 1); }

  Result<void> expect_line_end() {
    skip_blanks();
    if (peek() == '#' && p_ > 0 && blank(t_[p_ - 1])) skip_blanks_and_comment();
    if (!at_end() && peek() != '\n') {
      if (peek() == ':') return error("unexpected ':' (a mapping value is not allowed here)");
      return error("unexpected content after the value");
    }
    return {};
  }

  /// End of a quoted string on this line starting at `at`, or npos.
  [[nodiscard]] std::size_t quoted_end_on_line(std::size_t at) const noexcept {
    const char quote = t_[at];
    for (std::size_t i = at + 1; i < t_.size() && t_[i] != '\n'; ++i) {
      if (quote == '"' && t_[i] == '\\') {
        ++i;
        continue;
      }
      if (t_[i] == quote) {
        if (quote == '\'' && i + 1 < t_.size() && t_[i + 1] == '\'') {
          ++i;
          continue;
        }
        return i + 1;
      }
    }
    return std::string_view::npos;
  }

  /// Does an implicit mapping key (followed by ':' and a blank) start at `at`
  /// on this line?
  [[nodiscard]] bool implicit_key_ahead(std::size_t at) const noexcept {
    std::size_t i = at;
    if (i >= t_.size() || t_[i] == '\n' || t_[i] == '#') return false;
    // Properties of the key.
    while (i < t_.size() && (t_[i] == '&' || t_[i] == '!')) {
      while (i < t_.size() && !blank_or_end(i)) ++i;
      while (i < t_.size() && blank(t_[i])) ++i;
    }
    if (i >= t_.size()) return false;
    const char c = t_[i];
    if (c == '"' || c == '\'') {
      i = quoted_end_on_line(i);
      if (i == std::string_view::npos) return false;
    } else if (c == '[' || c == '{') {
      int nesting = 0;
      for (; i < t_.size() && t_[i] != '\n'; ++i) {
        const char d = t_[i];
        if (d == '"' || d == '\'') {
          const std::size_t end = quoted_end_on_line(i);
          if (end == std::string_view::npos) return false;
          i = end - 1;
        } else if (d == '[' || d == '{') {
          ++nesting;
        } else if (d == ']' || d == '}') {
          if (--nesting == 0) {
            ++i;
            break;
          }
        }
      }
      if (nesting != 0) return false;
    } else if (c == '*') {
      while (i < t_.size() && !blank_or_end(i)) ++i;
    } else {
      if ((c == '-' || c == '?' || c == ':') && blank_or_end(i + 1)) return c == ':' ? false : false;
      for (; i < t_.size() && t_[i] != '\n'; ++i) {
        if (t_[i] == ':' && blank_or_end(i + 1)) return true;
        if (t_[i] == '#' && i > at && blank(t_[i - 1])) return false;
      }
      return false;
    }
    while (i < t_.size() && blank(t_[i])) ++i;
    return i < t_.size() && t_[i] == ':' && blank_or_end(i + 1);
  }

  /// A key in block context: a single-line scalar, alias or flow collection.
  Result<std::pair<std::string, Json>> parse_implicit_key(Properties props, std::size_t depth) {
    if (peek() == '&' || peek() == '!') {
      CVT_TRY_ASSIGN(Properties more, parse_properties());
      if (props.anchor.empty()) props.anchor = std::move(more.anchor);
      if (props.tag.empty()) {
        props.tag = std::move(more.tag);
        props.tag_at = more.tag_at;
      }
    }
    const char c = peek();
    const std::size_t at = p_;
    Json key;
    std::string text;
    if (c == '*') {
      CVT_TRY_ASSIGN(key, parse_alias());
    } else if (c == '"' || c == '\'') {
      CVT_TRY_ASSIGN(text, c == '"' ? parse_double_quoted() : parse_single_quoted());
      CVT_TRY_ASSIGN(key, apply_scalar_tag(props, text, false, at));
      key = finish_node(props, std::move(key)).value();
    } else if (c == '[' || c == '{') {
      CVT_TRY_ASSIGN(key, parse_flow_collection(depth + 1));
      key = finish_node(props, std::move(key)).value();
    } else {
      CVT_TRY_ASSIGN(text, parse_plain(std::numeric_limits<int>::max(), false));
      CVT_TRY_ASSIGN(key, apply_scalar_tag(props, text, true, at));
      key = finish_node(props, std::move(key)).value();
      return std::pair<std::string, Json>{key.is_string() ? key.as_string() : text, std::move(key)};
    }
    return std::pair<std::string, Json>{key_text(key), std::move(key)};
  }

  static std::string key_text(const Json& key) {
    if (key.is_string()) return key.as_string();
    if (key.is_null()) return "null";
    if (key.is_bool()) return key.as_bool() ? "true" : "false";
    if (key.is_integer()) return format_integer(key);
    if (key.is_float()) return format_double(key.as_double());
    if (const TagKind kind = tag_kind(key); kind == TagKind::Date || kind == TagKind::BigInt || kind == TagKind::Decimal) {
      return std::string(tag_text(key));
    }
    return util::dump_json(key);
  }

  struct Entry {
    std::string key;
    Json value;
    bool merge = false;
    std::size_t at = 0;
  };

  Result<Json> build_mapping(std::vector<Entry> entries) {
    Json map = Json::object();
    bool has_merge = false;
    for (const Entry& entry : entries) has_merge = has_merge || entry.merge;
    if (!has_merge) {
      for (Entry& entry : entries) {
        if (auto put = put_member(map, std::move(entry.key), std::move(entry.value), policy_); !put) {
          return error_at(entry.at, put.error().message);
        }
      }
      return map;
    }
    // Merge keys: explicit keys win, earlier merge sources win over later ones.
    std::vector<std::string_view> explicit_keys;
    for (const Entry& entry : entries) {
      if (!entry.merge) explicit_keys.push_back(entry.key);
    }
    for (Entry& entry : entries) {
      if (!entry.merge) {
        if (auto put = put_member(map, std::move(entry.key), std::move(entry.value), policy_); !put) {
          return error_at(entry.at, put.error().message);
        }
        continue;
      }
      std::vector<const Json*> sources;
      if (entry.value.is_object()) {
        sources.push_back(&entry.value);
      } else if (entry.value.is_array()) {
        for (const Json& source : entry.value.elements()) {
          if (!source.is_object()) return error_at(entry.at, "a merge key (<<) needs mappings to merge");
          sources.push_back(&source);
        }
      } else {
        return error_at(entry.at, "a merge key (<<) needs a mapping or a sequence of mappings");
      }
      for (const Json* source : sources) {
        for (const auto& member : source->items()) {
          if (std::ranges::find(explicit_keys, member.key()) != explicit_keys.end()) continue;
          map.insert(member.key(), member.value());
        }
      }
    }
    return map;
  }

  Result<Json> parse_block_mapping(int indent, std::size_t depth, Properties first_key_props) {
    std::vector<Entry> entries;
    bool first = true;
    while (true) {
      Entry entry;
      entry.at = p_;
      if (peek() == '?' && blank_or_end(p_ + 1)) {
        ++p_;
        CVT_TRY_ASSIGN(Json key, parse_block_node(indent, depth + 1, false));
        entry.key = key_text(key);
        skip_space_lines();
        if (!at_end() && column() == indent && peek() == ':' && blank_or_end(p_ + 1)) {
          ++p_;
          CVT_TRY_ASSIGN(entry.value, parse_block_node(indent, depth + 1, true));
        }
      } else {
        if (!implicit_key_ahead(p_)) {
          if (peek() == '-' && blank_or_end(p_ + 1)) return error("a sequence item cannot appear inside a mapping here");
          return error("expected a mapping key followed by ':'");
        }
        const bool plain_merge = merge_keys_ && t_.substr(p_, 2) == "<<" && (first_key_props.empty() || !first);
        CVT_TRY_ASSIGN(auto key, parse_implicit_key(first ? std::move(first_key_props) : Properties{}, depth));
        entry.key = std::move(key.first);
        entry.merge = plain_merge && entry.key == "<<";
        skip_blanks();
        if (peek() != ':') return error("expected ':' after the mapping key");
        ++p_;
        CVT_TRY_ASSIGN(entry.value, parse_block_node(indent, depth + 1, true));
      }
      entries.push_back(std::move(entry));
      if (entries.size() % 4096 == 0) {
        CVT_TRY(ctx_.checkpoint(static_cast<double>(p_) / static_cast<double>(t_.size() + 1)));
      }
      first = false;
      skip_space_lines();
      if (at_end() || at_marker("---") || at_marker("...")) break;
      const int col = column();
      if (col < indent) break;
      if (col > indent) return error("bad indentation of a mapping entry");
      CVT_TRY(check_indentation_tabs());
      if (peek() == '-' && blank_or_end(p_ + 1)) break;  // a compact sequence of the parent
    }
    return build_mapping(std::move(entries));
  }

  Result<Json> parse_block_sequence(int indent, std::size_t depth) {
    Json seq = Json::array();
    while (true) {
      ++p_;  // '-'
      CVT_TRY_ASSIGN(Json item, parse_block_node(indent, depth + 1, false));
      seq.push_back(std::move(item));
      skip_space_lines();
      if (at_end() || at_marker("---") || at_marker("...")) break;
      const int col = column();
      if (col < indent) break;
      if (col > indent) return error("bad indentation of a sequence item");
      CVT_TRY(check_indentation_tabs());
      if (!(peek() == '-' && blank_or_end(p_ + 1))) {
        if (implicit_key_ahead(p_)) break;  // the parent mapping continues (compact sequence)
        return error("expected '- ' for the next sequence item");
      }
    }
    return seq;
  }

  // ------------------------------------------------------------------ scalars

  /// Folds the line break(s) consumed at the current position: returns the
  /// number of breaks and leaves p_ at the first non-blank of the next
  /// non-empty line.
  std::size_t consume_breaks() {
    std::size_t breaks = 0;
    while (!at_end()) {
      if (peek() == '\n') {
        ++breaks;
        ++p_;
      } else if (blank(peek())) {
        ++p_;
      } else {
        break;
      }
    }
    return breaks;
  }

  static void append_fold(std::string& out, std::size_t breaks) {
    if (breaks == 1) {
      out.push_back(' ');
    } else {
      out.append(breaks - 1, '\n');
    }
  }

  /// Plain scalar. In block context continuation lines must be indented more
  /// than `parent`; `parent` = INT_MAX means a single-line (key) scalar.
  Result<std::string> parse_plain(int parent, bool flow) {
    std::string out;
    while (true) {
      // One line.
      const std::size_t start = p_;
      std::size_t end = p_;
      bool stop = false;
      while (!at_end() && peek() != '\n') {
        const char c = peek();
        if (c == ':' && (blank_or_end(p_ + 1) || (flow && flow_indicator(peek(1))))) {
          stop = true;
          break;
        }
        if (c == '#' && p_ > start && blank(t_[p_ - 1])) {
          stop = true;
          break;
        }
        if (c == '#' && p_ == start && !out.empty()) {
          stop = true;
          break;
        }
        if (flow && flow_indicator(c)) {
          stop = true;
          break;
        }
        ++p_;
        if (!blank(c)) end = p_;
      }
      out.append(t_.substr(start, end - start));
      if (end < p_) p_ = end;  // leave trailing blanks unconsumed
      if (stop || at_end() || parent == std::numeric_limits<int>::max()) break;
      // Continuation?
      const std::size_t save = p_;
      skip_blanks();
      if (peek() != '\n') {
        p_ = save;
        break;
      }
      const std::size_t breaks = consume_breaks();
      const bool continues = !at_end() && !at_marker_at(p_) && peek() != '#' &&
                             (flow || static_cast<int>(p_ - line_start(p_)) > parent) &&
                             !(flow && (flow_indicator(peek()) || (peek() == ':' && blank_or_end(p_ + 1))));
      if (!continues) {
        p_ = save;
        break;
      }
      if (!flow) {
        for (std::size_t i = line_start(p_); i < p_; ++i) {
          if (t_[i] == '\t' && i == line_start(p_)) break;
        }
      }
      append_fold(out, breaks);
    }
    return out;
  }

  Result<std::string> parse_single_quoted() {
    const std::size_t start = p_;
    ++p_;
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated single-quoted string");
      const char c = peek();
      if (c == '\'') {
        if (peek(1) == '\'') {
          out.push_back('\'');
          p_ += 2;
          continue;
        }
        ++p_;
        return out;
      }
      if (c == '\n' || blank(c)) {
        const std::size_t ws_start = p_;
        skip_blanks();
        if (peek() == '\n') {
          const std::size_t breaks = consume_breaks();
          if (at_marker_at(p_)) return error("a document marker cannot appear inside a quoted string");
          append_fold(out, breaks);
        } else {
          out.append(t_.substr(ws_start, p_ - ws_start));
        }
        continue;
      }
      out.push_back(c);
      ++p_;
    }
  }

  Result<std::uint32_t> hex_escape(std::size_t digits) {
    if (p_ + digits > t_.size()) return error("incomplete escape sequence");
    const auto value = util::parse_int(t_.substr(p_, digits), 16);
    if (!value || t_.substr(p_, digits).find_first_of("+-") != std::string_view::npos) {
      return error("invalid hexadecimal escape");
    }
    p_ += digits;
    return static_cast<std::uint32_t>(*value);
  }

  Result<std::string> parse_double_quoted() {
    const std::size_t start = p_;
    ++p_;
    std::string out;
    while (true) {
      if (at_end()) return error_at(start, "unterminated double-quoted string");
      const char c = peek();
      if (c == '"') {
        ++p_;
        return out;
      }
      if (c == '\\') {
        ++p_;
        const char e = peek();
        ++p_;
        switch (e) {
          case '0': out.push_back('\0'); break;
          case 'a': out.push_back('\a'); break;
          case 'b': out.push_back('\b'); break;
          case 't':
          case '\t': out.push_back('\t'); break;
          case 'n': out.push_back('\n'); break;
          case 'v': out.push_back('\v'); break;
          case 'f': out.push_back('\f'); break;
          case 'r': out.push_back('\r'); break;
          case 'e': out.push_back('\x1B'); break;
          case ' ': out.push_back(' '); break;
          case '"': out.push_back('"'); break;
          case '/': out.push_back('/'); break;
          case '\\': out.push_back('\\'); break;
          case 'N': util::append_utf8(out, 0x85); break;
          case '_': util::append_utf8(out, 0xA0); break;
          case 'L': util::append_utf8(out, 0x2028); break;
          case 'P': util::append_utf8(out, 0x2029); break;
          case 'x':
          case 'u':
          case 'U': {
            CVT_TRY_ASSIGN(std::uint32_t cp, hex_escape(e == 'x' ? 2 : e == 'u' ? 4 : 8));
            if (e == 'u' && cp >= 0xD800 && cp <= 0xDBFF && t_.substr(p_, 2) == "\\u") {
              const std::size_t save = p_;
              p_ += 2;
              auto low = hex_escape(4);
              if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
              } else {
                p_ = save;
              }
            }
            if (!util::is_scalar_value(static_cast<char32_t>(cp))) {
              ++replaced_;
              cp = util::replacement_character;
            }
            util::append_utf8(out, static_cast<char32_t>(cp));
            break;
          }
          case '\n': {
            // Escaped line break: join without a space.
            skip_blanks();
            while (peek() == '\n') {
              out.push_back('\n');
              ++p_;
              skip_blanks();
            }
            break;
          }
          default: return error_at(p_ - 2, "invalid escape sequence in a double-quoted string");
        }
        continue;
      }
      if (c == '\n' || blank(c)) {
        const std::size_t ws_start = p_;
        skip_blanks();
        if (peek() == '\n') {
          const std::size_t breaks = consume_breaks();
          if (at_marker_at(p_)) return error("a document marker cannot appear inside a quoted string");
          append_fold(out, breaks);
        } else {
          out.append(t_.substr(ws_start, p_ - ws_start));
        }
        continue;
      }
      out.push_back(c);
      ++p_;
    }
  }

  /// Literal (|) or folded (>) block scalar; `parent` is the indentation of
  /// the enclosing block collection (-1 at the top level).
  Result<std::string> parse_block_scalar(int parent) {
    const bool literal = peek() == '|';
    ++p_;
    int chomping = 0;  // -1 strip, 0 clip, +1 keep
    int increment = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = peek();
      if ((c == '+' || c == '-') && chomping == 0) {
        chomping = c == '+' ? 1 : -1;
        ++p_;
      } else if (c >= '1' && c <= '9' && increment == 0) {
        increment = c - '0';
        ++p_;
      } else if (c == '0') {
        return error("the indentation indicator of a block scalar must be 1 to 9");
      }
    }
    skip_blanks();
    if (peek() == '#' && blank(t_[p_ - 1])) skip_blanks_and_comment();
    if (!at_end() && peek() != '\n') return error("unexpected text after the block scalar indicator");
    if (!at_end()) ++p_;  // the break ending the header

    int indent = 0;
    if (increment > 0) indent = parent >= 0 ? parent + increment : increment;
    std::string out;
    std::string leading_break;
    std::string trailing_breaks;
    // Leading empty lines and indentation detection.
    const auto scan_breaks = [&]() -> Result<void> {
      int max_indent = 0;
      while (true) {
        int col = 0;
        while ((indent == 0 || col < indent) && peek() == ' ') {
          ++p_;
          ++col;
        }
        if (col > max_indent) max_indent = col;
        if ((indent == 0 || col < indent) && peek() == '\t') {
          // A tab inside the indentation of an otherwise empty line is fine;
          // before content it is an error.
          std::size_t look = p_;
          while (look < t_.size() && blank(t_[look])) ++look;
          if (look < t_.size() && t_[look] != '\n') return error("tab characters cannot be used for indentation");
        }
        if (peek() != '\n') break;
        ++p_;
        trailing_breaks.push_back('\n');
      }
      if (indent == 0) {
        indent = std::max({max_indent, parent + 1, 1});
      }
      return {};
    };
    CVT_TRY(scan_breaks());
    bool trailing_blank = false;
    bool leading_blank = false;
    while (!at_end() && column() == indent && !at_marker_at(line_start(p_)) ) {
      // Content line.
      trailing_blank = blank(peek());
      if (!literal && !leading_break.empty() && leading_break[0] == '\n' && !leading_blank && !trailing_blank) {
        if (trailing_breaks.empty()) out.push_back(' ');
        leading_break.clear();
      } else {
        out += leading_break;
        leading_break.clear();
      }
      out += trailing_breaks;
      trailing_breaks.clear();
      leading_blank = blank(peek());
      const std::size_t start = p_;
      while (!at_end() && peek() != '\n') ++p_;
      out.append(t_.substr(start, p_ - start));
      if (at_end()) break;
      ++p_;
      leading_break = "\n";
      CVT_TRY(scan_breaks());
      if (at_marker_at(line_start(p_)) && column() == 0) break;
    }
    if (chomping != -1) out += leading_break;
    if (chomping == 1) out += trailing_breaks;
    // Leave p_ at the start of the line that ended the scalar.
    if (!at_end()) p_ = line_start(p_);
    return out;
  }

  // ------------------------------------------------------------------ flow collections

  bool skip_flow_space() {
    bool crossed = false;
    while (!at_end()) {
      const char c = peek();
      if (blank(c)) {
        ++p_;
      } else if (c == '\n') {
        ++p_;
        crossed = true;
      } else if (c == '#' && (blank(t_[p_ - 1]) || t_[p_ - 1] == '\n')) {
        while (!at_end() && peek() != '\n') ++p_;
      } else {
        break;
      }
    }
    return crossed;
  }

  Result<Json> parse_flow_node(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the YAML data is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    Properties props;
    if (peek() == '&' || peek() == '!') {
      CVT_TRY_ASSIGN(props, parse_properties());
      skip_flow_space();
    }
    const char c = peek();
    const std::size_t at = p_;
    if (at_end()) return error("unterminated flow collection");
    if (at_marker_at(p_)) return error("a document marker cannot appear inside a flow collection");
    if (c == '*') {
      if (!props.empty()) return error("an alias cannot have properties");
      return parse_alias();
    }
    if (c == '[' || c == '{') {
      if (!props.anchor.empty()) open_anchors_.push_back(props.anchor);
      auto value = parse_flow_collection(depth);
      if (!props.anchor.empty()) open_anchors_.pop_back();
      if (!value) return std::unexpected(std::move(value).error());
      CVT_TRY_ASSIGN(Json tagged, apply_collection_tag(props, std::move(*value)));
      return finish_node(props, std::move(tagged));
    }
    std::string text;
    bool plain = false;
    if (c == '"') {
      CVT_TRY_ASSIGN(text, parse_double_quoted());
    } else if (c == '\'') {
      CVT_TRY_ASSIGN(text, parse_single_quoted());
    } else if (flow_indicator(c) || (c == ':' && (blank_or_end(p_ + 1) || flow_indicator(peek(1))))) {
      plain = true;  // empty node
    } else {
      if ((c == '-' || c == '?') && (blank_or_end(p_ + 1) || flow_indicator(peek(1)))) {
        return error(std::format("unexpected '{}' in a flow collection", c));
      }
      CVT_TRY_ASSIGN(text, parse_plain(-1, true));
      plain = true;
    }
    CVT_TRY_ASSIGN(Json value, apply_scalar_tag(props, std::move(text), plain, at));
    return finish_node(props, std::move(value));
  }

  /// After a flow node: is there a ':' making it a key? (JSON-like keys - quoted
  /// or flow collections - may be followed directly by ':'.)
  bool flow_colon_follows(bool json_like) {
    const std::size_t save = p_;
    skip_flow_space();
    if (peek() == ':' && (json_like || blank_or_end(p_ + 1) || flow_indicator(peek(1)))) return true;
    p_ = save;
    return false;
  }

  Result<Json> parse_flow_collection(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the YAML data is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    const bool mapping = peek() == '{';
    const char close = mapping ? '}' : ']';
    const std::size_t start = p_;
    ++p_;
    std::vector<Entry> entries;
    Json seq = Json::array();
    while (true) {
      skip_flow_space();
      if (at_end()) return error_at(start, std::format("unterminated flow {}", mapping ? "mapping" : "sequence"));
      if (peek() == close) {
        ++p_;
        break;
      }
      if (peek() == ',') return error("unexpected ',' (empty entry) in a flow collection");
      bool explicit_key = false;
      if (peek() == '?' && (blank_or_end(p_ + 1) || flow_indicator(peek(1)))) {
        explicit_key = true;
        ++p_;
        skip_flow_space();
      }
      const std::size_t at = p_;
      const char first = peek();
      const bool json_like = first == '"' || first == '\'' || first == '[' || first == '{';
      Json node;
      if (explicit_key && (peek() == ':' || peek() == ',' || peek() == close)) {
        node = Json();
      } else {
        CVT_TRY_ASSIGN(node, parse_flow_node(depth + 1));
      }
      const bool is_pair = flow_colon_follows(json_like) || (explicit_key && peek() == ':');
      if (mapping || is_pair || explicit_key) {
        Json value;
        if (is_pair) {
          ++p_;  // ':'
          skip_flow_space();
          if (peek() == ',' || peek() == close) {
            value = Json();
          } else {
            CVT_TRY_ASSIGN(value, parse_flow_node(depth + 1));
          }
        }
        Entry entry{key_text(node), std::move(value), false, at};
        entry.merge = merge_keys_ && entry.key == "<<" && node.is_string() && first == '<';
        if (mapping) {
          entries.push_back(std::move(entry));
        } else {
          Json pair = Json::object();
          pair.set(entry.key, std::move(entry.value));
          seq.push_back(std::move(pair));
        }
      } else {
        seq.push_back(std::move(node));
      }
      skip_flow_space();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == close) {
        ++p_;
        break;
      }
      if (at_end()) return error_at(start, std::format("unterminated flow {}", mapping ? "mapping" : "sequence"));
      return error(std::format("expected ',' or '{}' in a flow collection", close));
    }
    if (mapping) return build_mapping(std::move(entries));
    return seq;
  }

  struct Anchor {
    Json value;
    std::uint64_t nodes = 0;
  };

  std::string_view t_;
  ReadContext& ctx_;
  Schema schema_;
  Schema active_schema_;
  bool forced_version_ = false;
  bool merge_keys_ = true;
  ReadPolicy policy_;
  std::size_t p_ = 0;
  mutable std::size_t cache_at_ = 0;
  mutable std::size_t cache_start_ = 0;
  std::map<std::string, std::string, std::less<>> handles_;
  std::unordered_map<std::string, Anchor> anchors_;
  std::vector<std::string> open_anchors_;
  std::uint64_t alias_nodes_ = 0;
  std::uint64_t alias_budget_ = 0;
  std::size_t unknown_tags_ = 0;

 public:
  std::size_t replaced_ = 0;
};

Result<Json> parse_yaml_text(std::string_view input, ReadContext& ctx) {
  std::string text = util::convert_line_endings(input, util::LineEnding::Lf);
  if (std::string_view(text).starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  Schema schema;
  const std::string version = ctx.options.get_enum("yaml_version", "auto", {"auto", "1.2", "1.1"});
  schema.yaml11 = version == "1.1";
  schema.timestamps = ctx.options.get_bool("timestamps", true);
  YamlParser parser(text, ctx, schema, version != "auto", ctx.options.get_bool("merge_keys", true));
  CVT_TRY_ASSIGN(std::vector<Json> documents, parser.parse_stream());
  if (parser.replaced_ > 0) {
    ctx.warn(std::format("{} escape(s) that are not Unicode scalar values were replaced with U+FFFD.", parser.replaced_));
  }
  return select_documents(std::move(documents), ctx.options, "YAML");
}

// =========================================================================== writer

/// Text that a YAML 1.1 or 1.2 reader would resolve to something other than
/// this very string when written plain.
bool resolves_to_non_string(std::string_view text) {
  Schema s12{false, true};
  Schema s11{true, true};
  if (!resolve_plain(text, s12).is_string() || !resolve_plain(text, s11).is_string()) return true;
  static constexpr std::string_view words[] = {"y", "Y", "n", "N", "yes", "no", "on", "off", "=", "<<", "~"};
  if (std::ranges::find(words, text) != std::end(words)) return true;
  // Number-like text in other spellings some readers accept (1_000, 0b1, +.5, 1e3).
  const char first = text.front();
  if (util::is_ascii_digit(first) || ((first == '-' || first == '+' || first == '.') && text.size() > 1)) {
    std::string cleaned;
    for (const char c : text) {
      if (c != '_') cleaned.push_back(c);
    }
    if (core_float(cleaned) || special_float(cleaned)) return true;
    if (resolve_int_11(text) || resolve_float_11(text)) return true;
  }
  return false;
}

bool printable(char32_t c) {
  return c == 0x09 || c == 0x0A || (c >= 0x20 && c <= 0x7E) || c == 0x85 || (c >= 0xA0 && c <= 0xD7FF) ||
         (c >= 0xE000 && c <= 0xFFFD && c != 0xFEFF) || (c >= 0x10000 && c <= 0x10FFFF);
}

bool all_printable(std::string_view text, bool allow_breaks) {
  std::size_t i = 0;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    if (!d.ok || !printable(d.code_point)) return false;
    if (!allow_breaks && (d.code_point == '\n' || d.code_point == 0x85 || d.code_point == 0x2028 ||
                          d.code_point == 0x2029)) {
      return false;
    }
    i += d.length;
  }
  return true;
}

bool plain_safe(std::string_view text, bool key) {
  if (text.empty()) return false;
  if (!all_printable(text, false)) return false;
  if (text.find('\t') != std::string_view::npos) return false;
  const char first = text.front();
  static constexpr std::string_view indicators = "-?:,[]{}#&*!|>'\"%@`";
  if (indicators.find(first) != std::string_view::npos) {
    // "-foo", "?x", ":x" are legal plain scalars, but quoting them is clearer.
    return false;
  }
  if (first == ' ' || text.back() == ' ' || text.back() == ':') return false;
  if (text.find(": ") != std::string_view::npos || text.find(" #") != std::string_view::npos) return false;
  if (text.find_first_of(",[]{}") != std::string_view::npos && key) return false;
  if (text.starts_with("---") || text.starts_with("...")) return false;
  if (util::decode_utf8(text, 0).code_point == 0xFEFF) return false;
  return !resolves_to_non_string(text);
}

void double_quote(std::string& out, std::string_view text) {
  out.push_back('"');
  std::size_t i = 0;
  while (i < text.size()) {
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    const char32_t c = d.ok ? d.code_point : util::replacement_character;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\0': out += "\\0"; break;
      case '\a': out += "\\a"; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\v': out += "\\v"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      case 0x1B: out += "\\e"; break;
      case 0x85: out += "\\N"; break;
      case 0xA0: out += "\\_"; break;
      case 0x2028: out += "\\L"; break;
      case 0x2029: out += "\\P"; break;
      default:
        if (!printable(c)) {
          out += c <= 0xFF ? std::format("\\x{:02X}", static_cast<unsigned>(c))
                 : c <= 0xFFFF ? std::format("\\u{:04X}", static_cast<unsigned>(c))
                               : std::format("\\U{:08X}", static_cast<unsigned>(c));
        } else if (d.ok) {
          out.append(text.substr(i, d.length));
        } else {
          util::append_utf8(out, c);
        }
    }
    i += d.length;
  }
  out.push_back('"');
}

/// YAML float spelling every reader accepts: a '.' in the mantissa and a
/// signed exponent ("1.0e+16").
std::string yaml_float_text(std::string text) {
  const std::size_t e = text.find_first_of("eE");
  std::string mantissa = text.substr(0, e);
  std::string exponent = e == std::string::npos ? "" : text.substr(e + 1);
  if (mantissa.find('.') == std::string::npos) mantissa += ".0";
  if (!exponent.empty() && exponent.front() != '-' && exponent.front() != '+') exponent.insert(0, "+");
  return exponent.empty() ? mantissa : mantissa + "e" + exponent;
}

struct YamlStyle {
  int indent = 2;
  bool sort_keys = false;
  bool indent_sequences = true;
  bool flow = false;
  bool block_literals = true;
};

class YamlWriter {
 public:
  explicit YamlWriter(const YamlStyle& style) : style_(style) {}

  std::string run(const Json& root) {
    write_root(root);
    return std::move(out_);
  }

  void write_root(const Json& value) {
    if (is_block_collection(value)) {
      write_block(value, 0);
    } else {
      write_inline(value, static_cast<std::size_t>(style_.indent), false);
      out_.push_back('\n');
    }
  }

 private:
  [[nodiscard]] bool is_block_collection(const Json& value) const {
    return !style_.flow && value.is_structured() && !value.empty() && !is_native_tag(value);
  }

  [[nodiscard]] static bool is_native_tag(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::Binary:
      case TagKind::Date:
      case TagKind::BigInt:
      case TagKind::Decimal:
      case TagKind::Float: return true;
      default: return false;
    }
  }

  void indent_to(std::size_t columns) { out_.append(columns, ' '); }

  void write_key(std::string_view key) {
    if (plain_safe(key, true) && key.size() <= 1024) {
      out_ += key;
    } else if (all_printable(key, false) && key.find('\'') == std::string_view::npos && key.size() <= 1024 &&
               key.find('\t') == std::string_view::npos) {
      out_.push_back('\'');
      out_ += key;
      out_.push_back('\'');
    } else {
      double_quote(out_, key);
    }
  }

  void write_string(std::string_view text, std::size_t column, bool in_flow) {
    if (plain_safe(text, in_flow) && !(in_flow && text.find_first_of(",[]{}") != std::string_view::npos)) {
      out_ += text;
      return;
    }
    const bool multiline = text.find('\n') != std::string_view::npos;
    const std::size_t first_content = text.find_first_not_of('\n');
    if (multiline && !in_flow && style_.block_literals && all_printable(text, true) &&
        first_content != std::string_view::npos && text[first_content] != ' ' && text[first_content] != '\t') {
      // Literal block scalar with the matching chomping indicator.
      std::size_t trailing = 0;
      while (trailing < text.size() && text[text.size() - 1 - trailing] == '\n') ++trailing;
      out_ += trailing == 0 ? "|-" : trailing == 1 ? "|" : "|+";
      const std::string_view body = text.substr(0, text.size() - trailing);
      std::size_t pos = 0;
      while (pos <= body.size()) {
        std::size_t end = body.find('\n', pos);
        if (end == std::string_view::npos) end = body.size();
        out_.push_back('\n');
        const std::string_view line = body.substr(pos, end - pos);
        if (!line.empty()) {
          indent_to(column);
          out_ += line;
        }
        pos = end + 1;
      }
      // `|+` keeps the extra breaks; the caller adds the final newline.
      for (std::size_t i = 1; i < trailing; ++i) out_.push_back('\n');
      return;
    }
    if (!multiline && all_printable(text, false) && text.find('\'') == std::string_view::npos &&
        text.find('\t') == std::string_view::npos && !text.empty() && !text.starts_with(' ') && !text.ends_with(' ')) {
      out_.push_back('\'');
      out_ += text;
      out_.push_back('\'');
      return;
    }
    double_quote(out_, text);
  }

  void write_scalar_number(const Json& value) {
    if (value.is_integer()) {
      out_ += format_integer(value);
      return;
    }
    const double d = value.as_double();
    if (std::isnan(d)) {
      out_ += ".nan";
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "-.inf" : ".inf";
    } else {
      out_ += yaml_float_text(format_double(d));
    }
  }

  /// A value that fits after "key: " or "- ": scalars, empty or flow collections, native tags.
  void write_inline(const Json& value, std::size_t column, bool in_flow) {
    switch (tag_kind(value)) {
      case TagKind::BigInt: out_ += tag_text(value); return;
      case TagKind::Decimal: out_ += yaml_float_text(std::string(tag_text(value))); return;
      case TagKind::Float: write_scalar_number(plain_value(value)); return;
      case TagKind::Date: {
        const std::string text(tag_text(value));
        if (const auto parsed = parse_datetime(text); parsed && parsed->kind != DateTime::Kind::Time && yaml_timestamp(text)) {
          out_ += text;
        } else {
          double_quote(out_, text);  // a time of day has no YAML type
        }
        return;
      }
      case TagKind::Binary: {
        const std::string& base64 = value[ir::data_tags::binary].as_string();
        if (in_flow || base64.size() <= 60) {
          out_ += "!!binary ";
          double_quote(out_, base64);
          return;
        }
        out_ += "!!binary |";
        for (std::size_t pos = 0; pos < base64.size(); pos += 76) {
          out_.push_back('\n');
          indent_to(column);
          out_ += std::string_view(base64).substr(pos, 76);
        }
        return;
      }
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "null"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64:
      case Json::Type::Double: write_scalar_number(value); return;
      case Json::Type::String: write_string(value.as_string(), column, in_flow); return;
      case Json::Type::Array: {
        out_.push_back('[');
        bool first = true;
        for (const Json& element : value.elements()) {
          if (!first) out_ += ", ";
          first = false;
          write_inline(element, column, true);
        }
        out_.push_back(']');
        return;
      }
      case Json::Type::Object: {
        out_.push_back('{');
        bool first = true;
        for (const Json::Member* member : members_of(value, style_.sort_keys)) {
          if (!first) out_ += ", ";
          first = false;
          if (plain_safe(member->key(), true)) {
            out_ += member->key();
          } else {
            double_quote(out_, member->key());
          }
          out_ += ": ";
          write_inline(member->value(), column, true);
        }
        out_.push_back('}');
        return;
      }
    }
  }

  /// A non-empty block collection starting at the beginning of a line at
  /// `column` (the first line may already be partly written for "- " items).
  void write_block(const Json& value, std::size_t column, bool continue_line = false) {
    const auto step = static_cast<std::size_t>(style_.indent);
    bool first = true;
    if (value.is_array()) {
      for (const Json& element : value.elements()) {
        if (!(first && continue_line)) indent_to(column);
        first = false;
        out_ += "- ";
        if (is_block_collection(element)) {
          write_block(element, column + 2, true);
        } else {
          write_inline(element, column + 2, false);
          out_.push_back('\n');
        }
      }
      return;
    }
    for (const Json::Member* member : members_of(value, style_.sort_keys)) {
      if (!(first && continue_line)) indent_to(column);
      first = false;
      write_key(member->key());
      const Json& child = member->value();
      if (is_block_collection(child)) {
        out_ += ":\n";
        const std::size_t child_column = child.is_array() && !style_.indent_sequences ? column : column + step;
        write_block(child, child_column);
      } else {
        out_ += ": ";
        write_inline(child, column + step, false);
        out_.push_back('\n');
      }
    }
  }

  const YamlStyle& style_;
  std::string out_;
};

Result<std::string> write_yaml(const Json& value, WriteContext& ctx) {
  YamlStyle style;
  style.indent = static_cast<int>(ctx.options.get_int("indent", 2, 1, 16));
  style.sort_keys = ctx.options.get_bool("sort_keys", false);
  style.indent_sequences = ctx.options.get_bool("indent_sequences", true);
  style.flow = ctx.options.get_enum("yaml_style", "block", {"block", "flow"}) == "flow";
  style.block_literals = ctx.options.get_bool("block_literals", true);
  const bool explicit_start = ctx.options.get_bool("explicit_start", false);
  const bool split = ctx.options.get_bool("split_documents", false);
  std::string out;
  if (split && value.is_array()) {
    for (const Json& document : value.elements()) {
      YamlWriter writer(style);
      out += "---";
      const std::string body = writer.run(document);
      out += body.starts_with('|') || body.starts_with('>') ? " " : "\n";
      out += body;
    }
    return out;
  }
  YamlWriter writer(style);
  if (explicit_start) out += "---\n";
  out += writer.run(value);
  return out;
}

int sniff_yaml(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data);
  const bool ext = has_extension(filename, {"yaml", "yml"});
  if (head.starts_with("%YAML")) return 90;
  if (ext) return 75;
  if (head.starts_with("---\n") || head.starts_with("--- ") || head.starts_with("---\r\n")) {
    // A document marker followed by "key: value" and no closing marker soon
    // after (which would make it Markdown front matter).
    const std::size_t nl = head.find('\n');
    const std::string_view next = nl == std::string_view::npos ? std::string_view() : head.substr(nl + 1);
    const std::size_t colon = next.find(": ");
    const bool mapping = colon != std::string_view::npos && colon < next.find('\n');
    return mapping && next.find("\n---") == std::string_view::npos ? 62 : 50;
  }
  return 0;
}

}  // namespace

void register_yaml(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "yaml",
      .name = "YAML",
      .description = "Human-friendly data serialisation language used for configuration files.",
      .category = Category::Data,
      .extensions = {"yaml", "yml"},
      .mime = "application/yaml",
      .popularity = 82,
      .signatures = {},
      .sniff = sniff_yaml,
  });
  std::vector<OptionSpec> read_options = common_read_options();
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  read_options.push_back(OptionSpec::choice("yaml_version", "YAML version", "auto", {"auto", "1.2", "1.1"},
                                            "Schema for untagged values: auto follows the %YAML directive (1.2 when "
                                            "absent); 1.1 also reads yes/no/on/off as booleans and 012 as octal.")
                             .advanced());
  read_options.push_back(OptionSpec::boolean("timestamps", "Recognise dates", true,
                                             "Read unquoted dates and times such as 2024-05-01 as date values."));
  read_options.push_back(OptionSpec::boolean("merge_keys", "Merge keys", true,
                                             "Apply '<<' merge keys (YAML 1.1 type, widely used).")
                             .advanced());
  add_codec(registry,
            {.format_id = "yaml",
             .read = text_reader(parse_yaml_text, "YAML"),
             .write = text_writer(write_yaml, "YAML"),
             .read_options = std::move(read_options),
             .write_options = {
                 OptionSpec::integer("indent", "Indentation", 2, 1, 16, "Spaces per nesting level."),
                 sort_keys_option(),
                 OptionSpec::boolean("indent_sequences", "Indent lists", true,
                                     "Indent list items under their key (off: '- ' at the key's column)."),
                 OptionSpec::choice("yaml_style", "Style", "block", {"block", "flow"},
                                    "Block style (one value per line) or flow style ({a: 1, b: [2, 3]}).")
                     .advanced(),
                 OptionSpec::boolean("block_literals", "Multi-line text as blocks", true,
                                     "Write text with line breaks as '|' blocks instead of quoted strings.")
                     .advanced(),
                 OptionSpec::boolean("explicit_start", "Start with ---", false,
                                     "Begin the document with an explicit '---' marker.")
                     .advanced(),
                 OptionSpec::boolean("split_documents", "One document per item", false,
                                     "Write each element of a top-level list as its own document of a stream.")
                     .advanced(),
             }});
}

}  // namespace cvt::data
