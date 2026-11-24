// Convertible core - data family: EDN and S-expressions.
//
// EDN (Extensible Data Notation): nil, booleans, strings, characters,
// integers (N suffix = arbitrary precision), floats (M suffix = exact
// decimal), keywords, symbols, lists, vectors, maps, sets, #inst -> $date,
// #uuid -> string, #_ discard, other tags keep their value (with a warning).
// Keywords become strings without the colon unless `keywords` is "keep".
// The writer turns object keys that are valid keywords into keywords.
//
// S-expressions (no single standard; the common Lisp/Scheme conventions):
// lists -> arrays, symbols and strings -> strings, numbers, nil/() -> null,
// t / #t -> true, #f -> false. A list of two-element lists or dotted pairs
// whose heads are symbols or strings (an association list) becomes an
// object unless `alists` is off. The writer writes objects as association
// lists ((key value) ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

// =========================================================================== EDN

class EdnParser {
 public:
  EdnParser(std::string_view text, const ReadContext& ctx)
      : t_(text), policy_(read_policy(ctx.options)),
        keep_colons_(ctx.options.get_enum("keywords", "strip", {"strip", "keep"}) == "keep") {}

  Result<std::vector<Json>> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    std::vector<Json> documents;
    while (true) {
      skip();
      if (at_end()) break;
      CVT_TRY_ASSIGN(auto value, form(0));
      if (value) documents.push_back(std::move(*value));
    }
    return documents;
  }

  std::size_t unknown_tags_ = 0;

 private:
  [[nodiscard]] bool at_end() const noexcept { return p_ >= t_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("EDN", t_, p_, message); }

  void skip() {
    while (!at_end()) {
      const char c = peek();
      if (util::is_ascii_space(c) || c == ',') {
        ++p_;
      } else if (c == ';') {
        while (!at_end() && peek() != '\n') ++p_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] static bool delimiter(char c) noexcept {
    return util::is_ascii_space(c) || c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '"' || c == ';' || c == '\0';
  }

  std::string token() {
    const std::size_t start = p_;
    while (!at_end() && !delimiter(peek())) ++p_;
    return std::string(t_.substr(start, p_ - start));
  }

  std::string keyword_text(std::string_view keyword) const {
    return keep_colons_ ? std::string(keyword) : std::string(keyword.substr(1));
  }

  Result<Json> collection(char close, std::size_t depth, bool map) {
    ++p_;
    std::vector<Json> items;
    while (true) {
      skip();
      if (at_end()) return error("unterminated collection");
      if (peek() == close) {
        ++p_;
        break;
      }
      CVT_TRY_ASSIGN(auto item, form(depth + 1));
      if (item) items.push_back(std::move(*item));
    }
    if (!map) {
      Json array = Json::array();
      for (Json& item : items) array.push_back(std::move(item));
      return array;
    }
    if (items.size() % 2 != 0) return error("a map needs an even number of forms");
    Json object = Json::object();
    for (std::size_t i = 0; i < items.size(); i += 2) {
      CVT_TRY(put_member(object, key_from_value(items[i]), std::move(items[i + 1]), policy_));
    }
    return object;
  }

  /// One form; nullopt for discarded forms (#_).
  Result<std::optional<Json>> form(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the EDN data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    skip();
    if (at_end()) return error("unexpected end of data");
    const char c = peek();
    switch (c) {
      case '(': return collection(')', depth, false);
      case '[': return collection(']', depth, false);
      case '{': return collection('}', depth, true);
      case ')':
      case ']':
      case '}': return error(std::format("unexpected '{}'", c));
      case '"': {
        CVT_TRY_ASSIGN(std::string text, string());
        return std::optional<Json>(Json(std::move(text)));
      }
      case '\\': return character();
      case '#': {
        if (peek(1) == '#') break;  // ##Inf, ##-Inf, ##NaN
        ++p_;
        if (peek() == '{') return collection('}', depth, false);
        if (peek() == '_') {
          ++p_;
          CVT_TRY(form(depth + 1));
          return std::optional<Json>();
        }
        const std::string tag = token();
        if (tag.empty()) return error("expected a tag after '#'");
        skip();
        CVT_TRY_ASSIGN(auto value, form(depth + 1));
        if (!value) return error("a tag needs a value");
        if (tag == "inst" && value->is_string()) {
          const auto parsed = parse_datetime(value->as_string());
          if (!parsed) return error("invalid #inst date");
          return std::optional<Json>(make_date(format_datetime(*parsed)));
        }
        if (tag != "uuid") ++unknown_tags_;
        return value;
      }
      default: break;
    }
    if (c == ':') {
      const std::string word = token();
      if (word.size() < 2) return error("empty keyword");
      return std::optional<Json>(Json(keyword_text(word)));
    }
    const std::string word = token();
    if (word.empty()) return error(std::format("unexpected character '{}'", c));
    if (word == "nil") return std::optional<Json>(Json());
    if (word == "true") return std::optional<Json>(Json(true));
    if (word == "false") return std::optional<Json>(Json(false));
    if (word == "##Inf" || word == "##-Inf" || word == "##NaN") {
      return std::optional<Json>(Json(word == "##NaN" ? std::numeric_limits<double>::quiet_NaN()
                                     : word == "##Inf" ? std::numeric_limits<double>::infinity()
                                                       : -std::numeric_limits<double>::infinity()));
    }
    const char first = word.front();
    if (util::is_ascii_digit(first) || ((first == '-' || first == '+') && word.size() > 1 && util::is_ascii_digit(word[1]))) {
      std::string number = word;
      const bool exact_int = number.ends_with('N');
      const bool exact_decimal = number.ends_with('M');
      if (exact_int || exact_decimal) number.pop_back();
      if (!decimal_parts(number) || (number.find_first_of(".eE") != std::string::npos && exact_int)) {
        return error(std::format("invalid number '{}'", word));
      }
      Json value = number_from_literal(number);
      if (exact_decimal && !value.is_integer() && tag_kind(value) != TagKind::BigInt) {
        return std::optional<Json>(make_decimal(std::string(tag_kind(value) == TagKind::Decimal ? tag_text(value) : number)));
      }
      return std::optional<Json>(std::move(value));
    }
    return std::optional<Json>(Json(word));  // a symbol
  }

  Result<std::string> string() {
    const std::size_t start = p_++;
    std::string out;
    while (true) {
      if (at_end()) {
        p_ = start;
        return error("unterminated string");
      }
      const char c = t_[p_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'n': out.push_back('\n'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case 'u': {
          const auto cp = p_ + 4 <= t_.size() ? util::parse_int(t_.substr(p_, 4), 16) : std::nullopt;
          if (!cp) return error("invalid \\u escape");
          p_ += 4;
          util::append_utf8(out, util::is_scalar_value(static_cast<char32_t>(*cp)) ? static_cast<char32_t>(*cp)
                                                                                   : util::replacement_character);
          break;
        }
        default: return error("invalid escape in a string");
      }
    }
  }

  Result<std::optional<Json>> character() {
    ++p_;
    const std::size_t start = p_;
    if (at_end()) return error("expected a character after '\\'");
    const util::Utf8Decoded first = util::decode_utf8(t_, p_);
    p_ += first.length;
    while (!at_end() && !delimiter(peek())) ++p_;
    const std::string_view name = t_.substr(start, p_ - start);
    std::string out;
    if (name == "newline") {
      out = "\n";
    } else if (name == "space") {
      out = " ";
    } else if (name == "tab") {
      out = "\t";
    } else if (name == "return") {
      out = "\r";
    } else if (name.size() == 5 && name[0] == 'u') {
      const auto cp = util::parse_int(name.substr(1), 16);
      if (!cp) return error("invalid \\u character");
      util::append_utf8(out, static_cast<char32_t>(*cp));
    } else if (name.size() == first.length) {
      out = std::string(name);
    } else {
      return error("invalid character literal");
    }
    return std::optional<Json>(Json(std::move(out)));
  }

  std::string_view t_;
  ReadPolicy policy_;
  bool keep_colons_ = false;
  std::size_t p_ = 0;
};

Result<Json> parse_edn(std::string_view text, ReadContext& ctx) {
  EdnParser parser(text, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, parser.parse());
  if (parser.unknown_tags_ > 0) ctx.warn(std::format("{} EDN tag(s) were dropped (their values were kept).", parser.unknown_tags_));
  return select_documents(std::move(documents), ctx.options, "EDN");
}

bool edn_keyword_name(std::string_view name) {
  if (name.empty() || util::is_ascii_digit(name[0]) || name[0] == ':' || name[0] == '#') return false;
  return std::ranges::all_of(name, [](char c) {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view("*+!-_?<>=./").find(c) != std::string_view::npos;
  });
}

void edn_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

class EdnWriter {
 public:
  EdnWriter(bool keyword_keys, bool sort) : keyword_keys_(keyword_keys), sort_(sort) {}

  std::string run(const Json& value) {
    write(value, 0);
    out_.push_back('\n');
    return std::move(out_);
  }

 private:
  void newline(std::size_t depth) {
    out_.push_back('\n');
    out_.append(depth, ' ');
  }

  void number(double d) {
    if (std::isnan(d)) {
      out_ += "##NaN";
    } else if (std::isinf(d)) {
      out_ += d < 0 ? "##-Inf" : "##Inf";
    } else {
      out_ += format_double(d);
    }
  }

  void write(const Json& value, std::size_t depth) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::BigInt: out_ += std::string(tag_text(value)) + "N"; return;
      case TagKind::Decimal: out_ += std::string(tag_text(value)) + "M"; return;
      case TagKind::Float: number(plain_value(value).as_double()); return;
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (parsed && parsed->kind == DateTime::Kind::Offset) {
          out_ += "#inst ";
          edn_string(out_, tag_text(value));
          return;
        }
        edn_string(out_, tag_text(value));
        return;
      }
      case TagKind::Symbol: out_ += tag_text(value); return;
      default: {
        const Json plain = plain_value(value);
        if (!plain.is_structured()) {
          write(plain, depth);
          return;
        }
        break;
      }
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "nil"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "true" : "false"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value) + (value.is_unsigned() ? "N" : ""); return;
      case Json::Type::Double: number(value.as_double()); return;
      case Json::Type::String: edn_string(out_, value.as_string()); return;
      case Json::Type::Array: {
        out_.push_back('[');
        bool first = true;
        for (const Json& element : value.elements()) {
          if (!first) out_.push_back(' ');
          first = false;
          write(element, depth + 1);
        }
        out_.push_back(']');
        return;
      }
      case Json::Type::Object: {
        out_.push_back('{');
        bool first = true;
        for (const Json::Member* member : members_of(value, sort_)) {
          if (!first) newline(depth + 1);
          first = false;
          if (keyword_keys_ && edn_keyword_name(member->key())) {
            out_ += ":" + member->key();
          } else {
            edn_string(out_, member->key());
          }
          out_.push_back(' ');
          write(member->value(), depth + 1);
        }
        out_.push_back('}');
        return;
      }
    }
  }

  bool keyword_keys_ = true;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_edn(const Json& value, WriteContext& ctx) {
  EdnWriter writer(ctx.options.get_bool("keyword_keys", true), ctx.options.get_bool("sort_keys", false));
  return writer.run(value);
}

// =========================================================================== S-expressions

struct SexpNode {
  enum class Kind : std::uint8_t { Atom, String, List, Dotted };
  Kind kind = Kind::Atom;
  std::string text;
  std::vector<SexpNode> items;  // List; Dotted: {car, cdr}
};

class SexpParser {
 public:
  SexpParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<std::vector<SexpNode>> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    std::vector<SexpNode> forms;
    while (true) {
      skip();
      if (p_ >= t_.size()) break;
      CVT_TRY_ASSIGN(SexpNode node, form(0));
      forms.push_back(std::move(node));
    }
    return forms;
  }

 private:
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("S-expression", t_, p_, message); }

  void skip() {
    while (p_ < t_.size()) {
      const char c = t_[p_];
      if (util::is_ascii_space(c)) {
        ++p_;
      } else if (c == ';') {
        while (p_ < t_.size() && t_[p_] != '\n') ++p_;
      } else if (c == '#' && p_ + 1 < t_.size() && t_[p_ + 1] == '|') {
        const std::size_t end = t_.find("|#", p_ + 2);
        p_ = end == std::string_view::npos ? t_.size() : end + 2;
      } else {
        break;
      }
    }
  }

  Result<SexpNode> form(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the S-expression is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    skip();
    if (p_ >= t_.size()) return error("unexpected end of data");
    const char c = t_[p_];
    if (c == '(' || c == '[') {
      const char close = c == '(' ? ')' : ']';
      ++p_;
      SexpNode list;
      list.kind = SexpNode::Kind::List;
      while (true) {
        skip();
        if (p_ >= t_.size()) return error("unterminated list");
        if (t_[p_] == close) {
          ++p_;
          return list;
        }
        if (t_[p_] == '.' && p_ + 1 < t_.size() && (util::is_ascii_space(t_[p_ + 1]) || t_[p_ + 1] == '(') && list.items.size() == 1) {
          ++p_;
          CVT_TRY_ASSIGN(SexpNode cdr, form(depth + 1));
          skip();
          if (p_ >= t_.size() || t_[p_] != close) return error("expected ')' after a dotted pair");
          ++p_;
          SexpNode pair;
          pair.kind = SexpNode::Kind::Dotted;
          pair.items.push_back(std::move(list.items[0]));
          pair.items.push_back(std::move(cdr));
          return pair;
        }
        CVT_TRY_ASSIGN(SexpNode item, form(depth + 1));
        list.items.push_back(std::move(item));
      }
    }
    if (c == ')' || c == ']') return error("unexpected ')'");
    if (c == '"') {
      ++p_;
      SexpNode node;
      node.kind = SexpNode::Kind::String;
      while (true) {
        if (p_ >= t_.size()) return error("unterminated string");
        const char d = t_[p_++];
        if (d == '"') return node;
        if (d == '\\' && p_ < t_.size()) {
          const char e = t_[p_++];
          node.text.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
          continue;
        }
        node.text.push_back(d);
      }
    }
    if (c == '\'') {  // quote: 'x -> x
      ++p_;
      return form(depth + 1);
    }
    const std::size_t start = p_;
    while (p_ < t_.size() && !util::is_ascii_space(t_[p_]) && t_[p_] != '(' && t_[p_] != ')' && t_[p_] != '[' &&
           t_[p_] != ']' && t_[p_] != '"' && t_[p_] != ';') {
      ++p_;
    }
    SexpNode atom;
    atom.text = std::string(t_.substr(start, p_ - start));
    return atom;
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Json sexp_atom(const SexpNode& node) {
  if (node.kind == SexpNode::Kind::String) return Json(node.text);
  const std::string& t = node.text;
  if (t == "nil" || t == "NIL") return Json();
  if (t == "t" || t == "T" || t == "#t" || t == "#true") return Json(true);
  if (t == "#f" || t == "#false") return Json(false);
  if (decimal_parts(t) && (util::is_ascii_digit(t.front()) || t.size() > 1)) {
    return number_from_literal(t);
  }
  return Json(t.starts_with(':') ? t.substr(1) : t);
}

Result<Json> sexp_value(const SexpNode& node, bool alists, const ReadPolicy& policy) {
  if (node.kind == SexpNode::Kind::Atom || node.kind == SexpNode::Kind::String) return sexp_atom(node);
  if (node.kind == SexpNode::Kind::Dotted) {
    Json pair = Json::array();
    for (const SexpNode& item : node.items) {
      CVT_TRY_ASSIGN(Json v, sexp_value(item, alists, policy));
      pair.push_back(std::move(v));
    }
    return pair;
  }
  if (node.items.empty()) return Json();
  const bool is_alist = alists && std::ranges::all_of(node.items, [](const SexpNode& item) {
    const bool pair = (item.kind == SexpNode::Kind::List && item.items.size() == 2) || item.kind == SexpNode::Kind::Dotted;
    return pair && (item.items[0].kind == SexpNode::Kind::Atom || item.items[0].kind == SexpNode::Kind::String);
  });
  if (is_alist) {
    Json object = Json::object();
    for (const SexpNode& item : node.items) {
      const Json key = sexp_atom(item.items[0]);
      CVT_TRY_ASSIGN(Json v, sexp_value(item.items[1], alists, policy));
      CVT_TRY(put_member(object, key_from_value(key), std::move(v), policy));
    }
    return object;
  }
  Json array = Json::array();
  for (const SexpNode& item : node.items) {
    CVT_TRY_ASSIGN(Json v, sexp_value(item, alists, policy));
    array.push_back(std::move(v));
  }
  return array;
}

Result<Json> parse_sexp(std::string_view text, ReadContext& ctx) {
  SexpParser parser(text, ctx);
  CVT_TRY_ASSIGN(std::vector<SexpNode> forms, parser.parse());
  const bool alists = ctx.options.get_bool("alists", true);
  const ReadPolicy policy = read_policy(ctx.options);
  std::vector<Json> documents;
  for (const SexpNode& node : forms) {
    CVT_TRY_ASSIGN(Json value, sexp_value(node, alists, policy));
    documents.push_back(std::move(value));
  }
  return select_documents(std::move(documents), ctx.options, "S-expression");
}

bool sexp_symbol(std::string_view text) {
  if (text.empty() || text == "nil" || text == "t" || text == "." || decimal_parts(text)) return false;
  return std::ranges::all_of(text, [](char c) {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view("-_*+/<>=!?.:%&$").find(c) != std::string_view::npos;
  });
}

class SexpWriter {
 public:
  explicit SexpWriter(bool sort) : sort_(sort) {}
  std::string run(const Json& value) {
    write(value, 0);
    out_.push_back('\n');
    return std::move(out_);
  }

 private:
  void string(std::string_view text) {
    out_.push_back('"');
    for (const char c : text) {
      if (c == '"' || c == '\\') out_.push_back('\\');
      if (c == '\n') {
        out_ += "\\n";
        continue;
      }
      out_.push_back(c);
    }
    out_.push_back('"');
  }

  void write(const Json& value, std::size_t depth) {
    if (tag_kind(value) != TagKind::None) {
      const Json plain = plain_value(value);
      if (tag_kind(value) == TagKind::BigInt || tag_kind(value) == TagKind::Decimal) {
        out_ += tag_text(value);
        return;
      }
      if (!plain.is_structured()) {
        write(plain, depth);
        return;
      }
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "nil"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "#t" : "#f"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return;
      case Json::Type::Double: {
        const double d = value.as_double();
        if (std::isfinite(d)) {
          out_ += format_double(d);
        } else {
          string(format_double(d));
        }
        return;
      }
      case Json::Type::String: string(value.as_string()); return;
      case Json::Type::Array: {
        out_.push_back('(');
        bool first = true;
        for (const Json& element : value.elements()) {
          if (!first) out_.push_back(' ');
          first = false;
          write(element, depth + 1);
        }
        out_.push_back(')');
        return;
      }
      case Json::Type::Object: {
        out_.push_back('(');
        bool first = true;
        for (const Json::Member* member : members_of(value, sort_)) {
          if (!first) {
            out_.push_back('\n');
            out_.append(depth + 1, ' ');
          }
          first = false;
          out_.push_back('(');
          if (sexp_symbol(member->key())) {
            out_ += member->key();
          } else {
            string(member->key());
          }
          out_.push_back(' ');
          write(member->value(), depth + 2);
          out_.push_back(')');
        }
        out_.push_back(')');
        return;
      }
    }
  }

  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_sexp(const Json& value, WriteContext& ctx) {
  SexpWriter writer(ctx.options.get_bool("sort_keys", false));
  return writer.run(value);
}

}  // namespace

void register_lisp_formats(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "edn",
      .name = "EDN",
      .description = "Extensible Data Notation, the data format of Clojure.",
      .category = Category::Data,
      .extensions = {"edn"},
      .mime = "application/edn",
      .popularity = 12,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"edn"}) ? 75 : 0; },
  });
  std::vector<OptionSpec> edn_read = common_read_options(true, false);
  edn_read.push_back(documents_option());
  edn_read.push_back(document_index_option());
  edn_read.push_back(OptionSpec::choice("keywords", "Keywords", "strip", {"strip", "keep"},
                                        "Read :keyword as \"keyword\" (strip) or as \":keyword\" (keep)."));
  add_codec(registry, {.format_id = "edn",
                       .read = text_reader(parse_edn, "EDN"),
                       .write = text_writer(write_edn, "EDN"),
                       .read_options = std::move(edn_read),
                       .write_options = {sort_keys_option(),
                                         OptionSpec::boolean("keyword_keys", "Keys as keywords", true,
                                                             "Write object keys that are valid keyword names as :keywords.")}});
  registry.add_format(FormatInfo{
      .id = "sexp",
      .name = "S-expression",
      .description = "Lisp-style nested lists of atoms.",
      .category = Category::Data,
      .extensions = {"sexp", "sexpr"},
      .mime = "text/x-sexp",
      .popularity = 8,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"sexp", "sexpr"}) ? 75 : 0; },
  });
  std::vector<OptionSpec> sexp_read = common_read_options(true, false);
  sexp_read.push_back(documents_option());
  sexp_read.push_back(document_index_option());
  sexp_read.push_back(OptionSpec::boolean("alists", "Association lists as objects", true,
                                          "Read lists of (key value) pairs as objects."));
  add_codec(registry, {.format_id = "sexp",
                       .read = text_reader(parse_sexp, "S-expression"),
                       .write = text_writer(write_sexp, "S-expression"),
                       .read_options = std::move(sexp_read),
                       .write_options = {sort_keys_option()},
                       .lossy = true});
}

}  // namespace cvt::data
