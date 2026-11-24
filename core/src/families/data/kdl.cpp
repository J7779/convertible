// Convertible core - data family: KDL documents (KDL 2.0 and 1.0 syntax).
//
// Reader: nodes with arguments, properties and children; identifier, quoted,
// raw (#"..."# and r"..."), and multi-line ("""...""") strings; numbers in
// all bases; #true/#false/#null/#inf/#-inf/#nan (and the 1.0 keywords);
// type annotations; slashdash and nested block comments; line continuations.
// Mapping to data (the writer reverses it):
//   * a document (or a children block) is an object of its nodes; repeated
//     node names collect into an array; a block of only "-" nodes is an array;
//   * a node with only arguments is its argument (one), an array (several)
//     or null (none); a node with properties or children is an object of its
//     properties and children, with its arguments under "-";
//   * the type annotation (array) or (object) forces that shape, which the
//     writer uses for empty and single-element arrays.
// Other type annotations are dropped with a warning.
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

struct KdlNode {
  std::string name;
  std::string type;
  std::vector<Json> args;
  std::vector<std::pair<std::string, Json>> props;
  std::vector<KdlNode> children;
  bool has_block = false;
};

class KdlParser {
 public:
  KdlParser(std::string_view text, const ReadContext& ctx) : t_(text), policy_(read_policy(ctx.options)) {}

  Result<std::vector<KdlNode>> parse() {
    if (t_.starts_with("\xEF\xBB\xBF")) p_ = 3;
    CVT_TRY_ASSIGN(std::vector<KdlNode> nodes, nodes_until('\0', 0));
    return nodes;
  }

  std::size_t dropped_types_ = 0;

 private:
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept { return p_ + ahead < t_.size() ? t_[p_ + ahead] : '\0'; }
  std::unexpected<Error> error(std::string_view message) const { return syntax_error("KDL", t_, p_, message); }

  Result<void> block_comment() {
    int nesting = 0;
    while (p_ < t_.size()) {
      if (t_.substr(p_, 2) == "/*") {
        ++nesting;
        p_ += 2;
      } else if (t_.substr(p_, 2) == "*/") {
        p_ += 2;
        if (--nesting == 0) return {};
      } else {
        ++p_;
      }
    }
    return error("unterminated comment");
  }

  /// Blanks (and comments, and escaped line breaks) within a line.
  Result<void> skip_inline() {
    while (p_ < t_.size()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || (static_cast<unsigned char>(c) == 0xC2 && peek(1) == '\xA0')) {
        p_ += static_cast<unsigned char>(c) == 0xC2 ? 2 : 1;
      } else if (t_.substr(p_, 2) == "/*") {
        CVT_TRY(block_comment());
      } else if (c == '\\') {
        ++p_;
        CVT_TRY(skip_inline());
        if (t_.substr(p_, 2) == "//") {
          while (p_ < t_.size() && peek() != '\n') ++p_;
        }
        if (peek() == '\n') ++p_;
      } else {
        break;
      }
    }
    return {};
  }

  Result<void> skip_lines() {
    while (true) {
      CVT_TRY(skip_inline());
      if (peek() == '\n' || peek() == ';') {
        ++p_;
      } else if (t_.substr(p_, 2) == "//") {
        while (p_ < t_.size() && peek() != '\n') ++p_;
      } else if (t_.starts_with("\xEF\xBB\xBF") && p_ == 0) {
        p_ = 3;
      } else {
        return {};
      }
    }
  }

  [[nodiscard]] static bool identifier_char(char c) {
    return !(util::is_ascii_space(c) || std::string_view("\\/(){}[];=\"#").find(c) != std::string_view::npos || c == '\0');
  }

  Result<std::string> quoted() {
    if (t_.substr(p_, 3) == "\"\"\"") return multiline();
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= t_.size()) return error("unterminated string");
      const char c = t_[p_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char e = peek();
      ++p_;
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case '"': out.push_back('"'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 's': out.push_back(' '); break;
        case 'u': {
          if (peek() != '{') return error("invalid \\u escape");
          const std::size_t close = t_.find('}', p_);
          if (close == std::string_view::npos || close - p_ > 7) return error("invalid \\u escape");
          const auto cp = util::parse_int(t_.substr(p_ + 1, close - p_ - 1), 16);
          if (!cp || *cp < 0 || !util::is_scalar_value(static_cast<char32_t>(*cp))) return error("invalid \\u escape");
          util::append_utf8(out, static_cast<char32_t>(*cp));
          p_ = close + 1;
          break;
        }
        default:
          if (util::is_ascii_space(e)) {  // whitespace escape
            while (p_ < t_.size() && util::is_ascii_space(peek())) ++p_;
            break;
          }
          return error("invalid escape");
      }
    }
  }

  Result<std::string> multiline() {
    p_ += 3;
    if (peek() == '\r') ++p_;
    if (peek() != '\n') return error("a multi-line string must start with a line break");
    ++p_;
    const std::size_t end = t_.find("\"\"\"", p_);
    if (end == std::string_view::npos) return error("unterminated multi-line string");
    std::string_view body = t_.substr(p_, end - p_);
    p_ = end + 3;
    // The last line holds only the indentation that every line loses.
    const std::size_t last = body.rfind('\n');
    const std::string_view indent = last == std::string_view::npos ? body : body.substr(last + 1);
    if (!util::trim(indent).empty()) return error("the closing \"\"\" must be on its own line");
    body = last == std::string_view::npos ? std::string_view() : body.substr(0, last);
    std::string out;
    std::size_t pos = 0;
    while (pos <= body.size() && !body.empty()) {
      std::size_t eol = body.find('\n', pos);
      if (eol == std::string_view::npos) eol = body.size();
      std::string_view line = body.substr(pos, eol - pos);
      if (line.ends_with('\r')) line.remove_suffix(1);
      if (line.starts_with(indent)) {
        line.remove_prefix(indent.size());
      } else if (!util::trim(line).empty()) {
        return error("inconsistent indentation in a multi-line string");
      } else {
        line = {};
      }
      std::string piece;
      std::string_view rest = line;
      // Escapes are processed in multi-line strings too.
      for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '\\' && i + 1 < rest.size()) {
          const char e = rest[++i];
          piece.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 's' ? ' ' : e);
        } else {
          piece.push_back(rest[i]);
        }
      }
      out += piece;
      if (eol < body.size()) out.push_back('\n');
      pos = eol + 1;
    }
    return out;
  }

  Result<std::string> raw_string() {
    // v2: #"..."#, v1: r"..." / r#"..."#.
    if (peek() == 'r') ++p_;
    std::size_t hashes = 0;
    while (peek() == '#') {
      ++hashes;
      ++p_;
    }
    if (peek() != '"') return error("expected '\"' in a raw string");
    const bool triple = t_.substr(p_, 3) == "\"\"\"";
    p_ += triple ? 3 : 1;
    const std::string close = (triple ? "\"\"\"" : "\"") + std::string(hashes, '#');
    const std::size_t end = t_.find(close, p_);
    if (end == std::string_view::npos) return error("unterminated raw string");
    std::string text(t_.substr(p_, end - p_));
    p_ = end + close.size();
    if (triple) {
      if (text.starts_with('\n')) text.erase(0, 1);
      const std::size_t last = text.rfind('\n');
      if (last != std::string::npos && util::trim(std::string_view(text).substr(last + 1)).empty()) {
        const std::string indent = text.substr(last + 1);
        text.resize(last);
        std::string out;
        for (const std::string_view line : util::split(text, '\n')) {
          if (!out.empty()) out.push_back('\n');
          out += line.starts_with(indent) ? line.substr(indent.size()) : line;
        }
        text = out;
      }
    }
    return text;
  }

  /// A string (identifier, quoted or raw) for names and property keys.
  Result<std::string> string_token() {
    const char c = peek();
    if (c == '"') return quoted();
    if (c == '#' || (c == 'r' && (peek(1) == '"' || peek(1) == '#'))) return raw_string();
    const std::size_t start = p_;
    while (p_ < t_.size() && identifier_char(peek())) ++p_;
    if (p_ == start) return error("expected an identifier");
    return std::string(t_.substr(start, p_ - start));
  }

  Result<Json> value() {
    const char c = peek();
    if (c == '"') {
      CVT_TRY_ASSIGN(std::string text, quoted());
      return Json(std::move(text));
    }
    if (c == '#') {
      const std::size_t start = p_;
      ++p_;
      const std::size_t word_start = p_;
      while (p_ < t_.size() && identifier_char(peek())) ++p_;
      const std::string_view word = t_.substr(word_start, p_ - word_start);
      if (word == "true") return Json(true);
      if (word == "false") return Json(false);
      if (word == "null") return Json();
      if (word == "inf") return Json(std::numeric_limits<double>::infinity());
      if (word == "-inf") return Json(-std::numeric_limits<double>::infinity());
      if (word == "nan") return Json(std::numeric_limits<double>::quiet_NaN());
      p_ = start;
      CVT_TRY_ASSIGN(std::string text, raw_string());
      return Json(std::move(text));
    }
    if (c == 'r' && (peek(1) == '"' || peek(1) == '#')) {
      CVT_TRY_ASSIGN(std::string text, raw_string());
      return Json(std::move(text));
    }
    const std::size_t start = p_;
    while (p_ < t_.size() && identifier_char(peek())) ++p_;
    const std::string_view word = t_.substr(start, p_ - start);
    if (word.empty()) return error("expected a value");
    if (word == "true") return Json(true);  // KDL 1.0 keywords
    if (word == "false") return Json(false);
    if (word == "null") return Json();
    const char first = word[0];
    const bool numeric = util::is_ascii_digit(first) ||
                         ((first == '-' || first == '+') && word.size() > 1 && util::is_ascii_digit(word[1])) ||
                         (first == '.' && word.size() > 1 && util::is_ascii_digit(word[1]));
    if (!numeric) return Json(std::string(word));  // identifier string (KDL 2.0)
    std::string digits;
    for (const char d : word) {
      if (d != '_') digits.push_back(d);
    }
    std::string_view body = digits;
    bool negative = false;
    if (body.starts_with('-') || body.starts_with('+')) {
      negative = body.front() == '-';
      body.remove_prefix(1);
    }
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'o' || body[1] == 'b')) {
      auto parsed = integer_from_digits(body.substr(2), body[1] == 'x' ? 16 : body[1] == 'o' ? 8 : 2, negative);
      if (!parsed) {
        p_ = start;
        return error("invalid number");
      }
      return std::move(*parsed);
    }
    if (!decimal_parts(digits)) {
      p_ = start;
      return error("invalid number");
    }
    return number_from_literal(digits);
  }

  Result<std::vector<KdlNode>> nodes_until(char close, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the KDL document is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    std::vector<KdlNode> nodes;
    while (true) {
      CVT_TRY(skip_lines());
      if (p_ >= t_.size()) {
        if (close != '\0') return error("unterminated children block");
        return nodes;
      }
      if (peek() == close) {
        ++p_;
        return nodes;
      }
      bool discard = false;
      if (t_.substr(p_, 2) == "/-") {
        p_ += 2;
        discard = true;
        CVT_TRY(skip_lines());
      }
      CVT_TRY_ASSIGN(KdlNode node, parse_node(depth));
      if (!discard) nodes.push_back(std::move(node));
    }
  }

  Result<std::string> annotation() {
    ++p_;  // (
    CVT_TRY(skip_inline());
    CVT_TRY_ASSIGN(std::string type, string_token());
    CVT_TRY(skip_inline());
    if (peek() != ')') return error("expected ')' after a type annotation");
    ++p_;
    CVT_TRY(skip_inline());
    return type;
  }

  Result<KdlNode> parse_node(std::size_t depth) {
    KdlNode node;
    if (peek() == '(') {
      CVT_TRY_ASSIGN(node.type, annotation());
    }
    CVT_TRY_ASSIGN(node.name, string_token());
    while (true) {
      const std::size_t before = p_;
      CVT_TRY(skip_inline());
      const char c = peek();
      if (p_ >= t_.size() || c == '\n' || c == ';' || c == '}' || t_.substr(p_, 2) == "//") break;
      if (p_ == before && c != '{' && t_.substr(p_, 2) != "/-") return error("expected white space between node entries");
      bool discard = false;
      if (t_.substr(p_, 2) == "/-") {
        p_ += 2;
        CVT_TRY(skip_inline());
        discard = true;
      }
      if (peek() == '{') {
        ++p_;
        CVT_TRY_ASSIGN(std::vector<KdlNode> children, nodes_until('}', depth + 1));
        if (!discard) {
          node.children = std::move(children);
          node.has_block = true;
        }
        continue;
      }
      std::string type;
      if (peek() == '(') {
        CVT_TRY_ASSIGN(type, annotation());
        if (!discard) ++dropped_types_;
      }
      // Property (key=value) or argument.
      const std::size_t entry = p_;
      bool is_property = false;
      if (peek() != '(') {
        const std::size_t save = p_;
        auto key = string_token();
        if (key && peek() == '=') {
          ++p_;
          CVT_TRY(skip_inline());
          if (peek() == '(') {
            CVT_TRY(annotation());
            if (!discard) ++dropped_types_;
          }
          CVT_TRY_ASSIGN(Json v, value());
          if (!discard) node.props.emplace_back(std::move(*key), std::move(v));
          is_property = true;
        } else {
          p_ = save;
        }
      }
      if (!is_property) {
        p_ = entry;
        CVT_TRY_ASSIGN(Json v, value());
        if (!discard) node.args.push_back(std::move(v));
      }
    }
    if (peek() == ';') ++p_;
    return node;
  }

  std::string_view t_;
  ReadPolicy policy_;
  std::size_t p_ = 0;
};

Json args_value(std::vector<Json> args) {
  if (args.empty()) return Json();
  if (args.size() == 1) return std::move(args[0]);
  Json array = Json::array();
  for (Json& arg : args) array.push_back(std::move(arg));
  return array;
}

Result<Json> nodes_value(std::vector<KdlNode>& nodes, const ReadPolicy& policy);

Result<Json> node_value(KdlNode& node, const ReadPolicy& policy) {
  if (node.type == "array") {
    Json array = Json::array();
    for (Json& arg : node.args) array.push_back(std::move(arg));
    if (node.has_block) {
      CVT_TRY_ASSIGN(Json children, nodes_value(node.children, policy));
      if (children.is_array()) {
        for (Json& element : children.elements()) array.push_back(std::move(element));
      } else if (!children.empty()) {
        array.push_back(std::move(children));
      }
    }
    return array;
  }
  if (node.props.empty() && !node.has_block && node.type != "object") return args_value(std::move(node.args));
  Json object = Json::object();
  for (auto& [key, value] : node.props) CVT_TRY(put_member(object, key, std::move(value), policy));
  if (!node.args.empty()) object.set("-", args_value(std::move(node.args)));
  if (node.has_block) {
    CVT_TRY_ASSIGN(Json children, nodes_value(node.children, policy));
    if (children.is_array()) {
      if (object.empty()) return children;
      object.set("-", std::move(children));
    } else {
      for (auto& member : children.items()) object.set(member.key(), std::move(member.value()));
    }
  }
  return object;
}

Result<Json> nodes_value(std::vector<KdlNode>& nodes, const ReadPolicy& policy) {
  const bool list = !nodes.empty() && std::ranges::all_of(nodes, [](const KdlNode& n) { return n.name == "-"; });
  if (list) {
    Json array = Json::array();
    for (KdlNode& node : nodes) {
      CVT_TRY_ASSIGN(Json value, node_value(node, policy));
      array.push_back(std::move(value));
    }
    return array;
  }
  Json object = Json::object();
  std::vector<std::string> repeated;
  for (KdlNode& node : nodes) {
    CVT_TRY_ASSIGN(Json value, node_value(node, policy));
    if (Json* existing = object.find(node.name)) {
      if (std::ranges::find(repeated, node.name) == repeated.end()) {
        Json first = std::move(*existing);
        *existing = Json::array({std::move(first)});
        repeated.push_back(node.name);
      }
      existing->push_back(std::move(value));
    } else {
      object.set(node.name, std::move(value));
    }
  }
  return object;
}

Result<Json> parse_kdl(std::string_view text, ReadContext& ctx) {
  KdlParser parser(text, ctx);
  CVT_TRY_ASSIGN(std::vector<KdlNode> nodes, parser.parse());
  if (parser.dropped_types_ > 0) ctx.warn(std::format("{} KDL type annotation(s) on values were dropped.", parser.dropped_types_));
  return nodes_value(nodes, read_policy(ctx.options));
}

// =========================================================================== writer

bool kdl_identifier(std::string_view text) {
  if (text.empty()) return false;
  static constexpr std::string_view keywords[] = {"true", "false", "null", "inf", "-inf", "nan"};
  if (std::ranges::find(keywords, text) != std::end(keywords)) return false;
  const char first = text[0];
  if (util::is_ascii_digit(first) || ((first == '-' || first == '+' || first == '.') && text.size() > 1 && util::is_ascii_digit(text[1]))) {
    return false;
  }
  if ((first == '-' || first == '+') && text.size() > 2 && text[1] == '.' && util::is_ascii_digit(text[2])) return false;
  return std::ranges::all_of(text, [](char c) {
    return static_cast<unsigned char>(c) > 0x20 && std::string_view("\\/(){}[];=\"#").find(c) == std::string_view::npos && c != 0x7F;
  });
}

void kdl_string(std::string& out, std::string_view text) {
  if (kdl_identifier(text)) {
    out += text;
    return;
  }
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          out += std::format("\\u{{{:x}}}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

class KdlWriter {
 public:
  KdlWriter(int indent, bool sort) : indent_(indent), sort_(sort) {}

  std::string run(const Json& value) {
    if (is_leaf(value)) {
      node("-", value, 0);
    } else {
      body(value, 0);
    }
    return std::move(out_);
  }

 private:
  static bool is_leaf(const Json& value) { return !value.is_structured() || tag_kind(value) != TagKind::None; }

  void scalar(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::BigInt:
      case TagKind::Decimal: out_ += tag_text(value); return;
      case TagKind::Float: scalar(plain_value(value)); return;
      default: {
        const Json plain = plain_value(value);
        if (!plain.is_structured()) {
          scalar(plain);
        } else {
          kdl_string(out_, util::dump_json(plain));
        }
        return;
      }
    }
    switch (value.type()) {
      case Json::Type::Null: out_ += "#null"; return;
      case Json::Type::Bool: out_ += value.as_bool() ? "#true" : "#false"; return;
      case Json::Type::Int64:
      case Json::Type::UInt64: out_ += format_integer(value); return;
      case Json::Type::Double: {
        const double d = value.as_double();
        if (std::isnan(d)) {
          out_ += "#nan";
        } else if (std::isinf(d)) {
          out_ += d < 0 ? "#-inf" : "#inf";
        } else {
          out_ += format_double(d);
        }
        return;
      }
      case Json::Type::String: kdl_string(out_, value.as_string()); return;
      default: return;
    }
  }

  void line_start(std::size_t depth) { out_.append(depth * static_cast<std::size_t>(indent_), ' '); }

  /// The members of an object, or the "-" items of an array, one node each.
  void body(const Json& value, std::size_t depth) {
    if (value.is_array()) {
      for (const Json& element : value.elements()) node("-", element, depth);
      return;
    }
    for (const Json::Member* member : members_of(value, sort_)) node(member->key(), member->value(), depth);
  }

  void node(std::string_view name, const Json& value, std::size_t depth) {
    line_start(depth);
    if (is_leaf(value)) {
      kdl_string(out_, name);
      out_.push_back(' ');
      scalar(value);
      out_.push_back('\n');
      return;
    }
    if (value.is_array()) {
      const bool scalars = std::ranges::all_of(value.elements(), [](const Json& e) { return is_leaf(e); });
      if (value.size() < 2 || !scalars) out_ += "(array)";
      kdl_string(out_, name);
      if (scalars) {
        for (const Json& element : value.elements()) {
          out_.push_back(' ');
          scalar(element);
        }
        out_.push_back('\n');
        return;
      }
      out_ += " {\n";
      body(value, depth + 1);
      line_start(depth);
      out_ += "}\n";
      return;
    }
    if (value.empty() || value.contains("-")) out_ += "(object)";
    kdl_string(out_, name);
    if (value.empty()) {
      out_ += " {}\n";
      return;
    }
    out_ += " {\n";
    for (const Json::Member* member : members_of(value, sort_)) {
      if (member->key() == "-") {
        // "-" is the argument slot of an object node; write it as a child list.
        node("-", member->value(), depth + 1);
        continue;
      }
      node(member->key(), member->value(), depth + 1);
    }
    line_start(depth);
    out_ += "}\n";
  }

  int indent_ = 4;
  bool sort_ = false;
  std::string out_;
};

Result<std::string> write_kdl(const Json& value, WriteContext& ctx) {
  KdlWriter writer(static_cast<int>(ctx.options.get_int("indent", 4, 0, 16)), ctx.options.get_bool("sort_keys", false));
  return writer.run(value);
}

}  // namespace

void register_kdl(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "kdl",
      .name = "KDL",
      .description = "The KDL document language: nodes with arguments, properties and children.",
      .category = Category::Data,
      .extensions = {"kdl"},
      .mime = "application/kdl",
      .popularity = 8,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"kdl"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "kdl",
                       .read = text_reader(parse_kdl, "KDL"),
                       .write = text_writer(write_kdl, "KDL"),
                       .read_options = common_read_options(true, false),
                       .write_options = {indent_option(4), sort_keys_option()},
                       .lossy = true});
}

}  // namespace cvt::data
