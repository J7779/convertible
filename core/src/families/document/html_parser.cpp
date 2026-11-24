// Convertible core - document family: tolerant HTML5 tokenizer and tree builder.
//
// Follows the WHATWG parsing model where it matters for documents: character
// references (named, numeric, legacy without ';', the C1 remapping), raw text
// and RCDATA elements, comments/doctype/CDATA/processing instructions,
// implied end tags (p, li, dd/dt, option, table parts, headings), implied
// table structure (tbody, tr), foster parenting of misplaced table content,
// misnested formatting elements (closed and re-opened) and the list of active
// formatting elements (bold that runs across paragraphs). It is not a
// conformance-checking parser: quirks that do not change the document
// content are simplified. Depth and node counts are bounded.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>

#include "families/document/doc_common.hpp"
#include "families/document/html_internal.hpp"
#include "util/text.hpp"

namespace cvt::document::html {

// =========================================================================== character references

namespace {

constexpr std::array<char32_t, 32> c1_replacements = {
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

bool is_alnum(char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c); }

// Decode one reference starting at text[i] == '&'. Returns the bytes consumed
// (0 when it is not a reference) and appends the value.
std::size_t decode_one(std::string_view text, std::size_t i, bool in_attribute, std::string& out) {
  const std::size_t start = i + 1;
  if (start >= text.size()) return 0;
  if (text[start] == '#') {
    std::size_t j = start + 1;
    bool hex = false;
    if (j < text.size() && (text[j] == 'x' || text[j] == 'X')) {
      hex = true;
      ++j;
    }
    const std::size_t digits_start = j;
    std::uint64_t value = 0;
    while (j < text.size()) {
      const char c = text[j];
      int digit = -1;
      if (util::is_ascii_digit(c)) {
        digit = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0) break;
      value = std::min<std::uint64_t>(value * (hex ? 16 : 10) + static_cast<std::uint64_t>(digit), 0x110000);
      ++j;
    }
    if (j == digits_start) return 0;
    if (j < text.size() && text[j] == ';') ++j;
    char32_t cp = static_cast<char32_t>(value);
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      cp = 0xFFFD;
    } else if (cp >= 0x80 && cp <= 0x9F) {
      cp = c1_replacements[cp - 0x80];
    }
    util::append_utf8(out, cp);
    return j - i;
  }
  std::size_t j = start;
  while (j < text.size() && j - start < 32 && is_alnum(text[j])) ++j;
  if (j == start) return 0;
  const std::string_view run = text.substr(start, j - start);
  if (j < text.size() && text[j] == ';') {
    const std::string with_semicolon = std::string(run) + ";";
    const std::string_view value = lookup_named_reference(with_semicolon);
    if (!value.empty()) {
      out.append(value);
      return j + 1 - i;
    }
  }
  // Longest legacy prefix without ';'.
  for (std::size_t length = run.size(); length >= 2; --length) {
    const std::string_view value = lookup_named_reference(run.substr(0, length));
    if (value.empty()) continue;
    if (in_attribute) {
      const std::size_t after = start + length;
      if (after < text.size() && (is_alnum(text[after]) || text[after] == '=')) return 0;
    }
    out.append(value);
    return length + 1;
  }
  return 0;
}

}  // namespace

std::string decode_references(std::string_view text, bool in_attribute) {
  if (text.find('&') == std::string_view::npos) return std::string(text);
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '&') {
      const std::size_t used = decode_one(text, i, in_attribute, out);
      if (used > 0) {
        i += used;
        continue;
      }
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

// =========================================================================== DOM helpers

std::string_view Dom::attribute(NodeId id, std::string_view name) const {
  for (const auto& [key, value] : nodes[id].attributes) {
    if (key == name) return value;
  }
  return {};
}

bool Dom::has_attribute(NodeId id, std::string_view name) const {
  for (const auto& [key, value] : nodes[id].attributes) {
    if (key == name) return true;
  }
  return false;
}

std::string Dom::text_content(NodeId id) const {
  std::string out;
  std::vector<NodeId> stack{id};
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    const Node& n = nodes[current];
    if (n.type == NodeType::Text) out.append(n.text);
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

NodeId Dom::find_first(std::string_view name, NodeId from) const {
  std::vector<NodeId> stack{from};
  while (!stack.empty()) {
    const NodeId current = stack.back();
    stack.pop_back();
    const Node& n = nodes[current];
    if (n.type == NodeType::Element && n.name == name) return current;
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
  }
  return no_node;
}

bool is_void_element(std::string_view name) noexcept {
  static constexpr std::array<std::string_view, 17> voids = {
      "area", "base",  "br",   "col",    "embed",  "hr",    "img",   "input",   "keygen",
      "link", "meta",  "param", "source", "track", "wbr",   "basefont", "frame"};
  return std::ranges::find(voids, name) != voids.end();
}

std::string escape_text(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') {
          if (c == '\r') out.push_back('\n');
          break;  // control characters are not allowed in HTML
        }
        out.push_back(c);
    }
  }
  return out;
}

std::string escape_attribute(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '"': out.append("&quot;"); break;
      case '\n': out.append("&#10;"); break;
      case '\t': out.append("&#9;"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) break;
        out.push_back(c);
    }
  }
  return out;
}

std::string serialize(const Dom& dom, NodeId id) {
  std::string out;
  // Iterative: (node, closing?) pairs.
  std::vector<std::pair<NodeId, bool>> stack{{id, false}};
  while (!stack.empty()) {
    const auto [current, closing] = stack.back();
    stack.pop_back();
    const Node& n = dom.nodes[current];
    if (closing) {
      out.append("</").append(n.name).append(">");
      continue;
    }
    if (n.type == NodeType::Text) {
      out.append(escape_text(n.text));
      continue;
    }
    if (n.type != NodeType::Element) continue;
    out.append("<").append(n.name);
    for (const auto& [key, value] : n.attributes) {
      if (key.starts_with("on")) continue;  // never carry event handlers
      out.append(" ").append(key).append("=\"").append(escape_attribute(value)).append("\"");
    }
    if (n.children.empty()) {
      out.append("/>");
      continue;
    }
    out.append(">");
    stack.emplace_back(current, true);
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.emplace_back(*it, false);
  }
  return out;
}

// =========================================================================== encoding prescan

namespace {

std::string charset_from_content(std::string_view content) {
  // "text/html; charset=iso-8859-2"
  const std::string lowered = util::to_lower(content);
  const std::size_t at = lowered.find("charset");
  if (at == std::string::npos) return {};
  std::size_t i = at + 7;
  while (i < lowered.size() && util::is_ascii_space(lowered[i])) ++i;
  if (i >= lowered.size() || lowered[i] != '=') return {};
  ++i;
  while (i < lowered.size() && util::is_ascii_space(lowered[i])) ++i;
  if (i < lowered.size() && (lowered[i] == '"' || lowered[i] == '\'')) ++i;
  std::size_t end = i;
  while (end < lowered.size() && !util::is_ascii_space(lowered[end]) && lowered[end] != ';' && lowered[end] != '"' &&
         lowered[end] != '\'') {
    ++end;
  }
  return lowered.substr(i, end - i);
}

}  // namespace

std::string prescan_charset(ByteView data) {
  if (const auto bom = util::detect_bom(data)) return std::string(util::encoding_name(bom->encoding));
  const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 4096)));
  // UTF-16 without BOM: "<\0" or "\0<".
  if (head.size() >= 4 && head[0] == '<' && head[1] == '\0' && head[3] == '\0') return "utf-16le";
  if (head.size() >= 4 && head[0] == '\0' && head[1] == '<' && head[2] == '\0') return "utf-16be";
  // XML declaration.
  if (head.starts_with("<?xml")) {
    const std::size_t end = head.find("?>");
    const std::string decl = util::to_lower(head.substr(0, end));
    const std::size_t at = decl.find("encoding");
    if (at != std::string::npos) {
      std::size_t q = decl.find_first_of("\"'", at);
      if (q != std::string::npos) {
        const std::size_t close = decl.find(decl[q], q + 1);
        if (close != std::string::npos) return decl.substr(q + 1, close - q - 1);
      }
    }
  }
  std::size_t i = 0;
  while (i < head.size()) {
    const std::size_t lt = head.find('<', i);
    if (lt == std::string_view::npos) break;
    if (head.substr(lt).starts_with("<!--")) {
      const std::size_t end = head.find("-->", lt + 4);
      if (end == std::string_view::npos) break;
      i = end + 3;
      continue;
    }
    if (!util::istarts_with(head.substr(lt), "<meta")) {
      i = lt + 1;
      continue;
    }
    const std::size_t gt = head.find('>', lt);
    if (gt == std::string_view::npos) break;
    const std::string tag = util::to_lower(head.substr(lt, gt - lt));
    const std::size_t cs = tag.find("charset=");
    if (cs != std::string::npos && tag.find("content=") == std::string::npos) {
      std::size_t v = cs + 8;
      if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) ++v;
      std::size_t end = v;
      while (end < tag.size() && tag[end] != '"' && tag[end] != '\'' && tag[end] != ' ' && tag[end] != '/' &&
             tag[end] != ';') {
        ++end;
      }
      if (end > v) return tag.substr(v, end - v);
    }
    if (tag.find("http-equiv") != std::string::npos && tag.find("content-type") != std::string::npos) {
      const std::size_t content = tag.find("content=");
      if (content != std::string::npos) {
        std::string found = charset_from_content(std::string_view(tag).substr(content));
        if (!found.empty()) return found;
      }
    }
    if (cs != std::string::npos) {
      std::string found = charset_from_content(std::string_view(tag).substr(cs > 0 ? cs - 1 : 0));
      if (!found.empty()) return found;
    }
    i = gt + 1;
  }
  return {};
}

// =========================================================================== tree builder

namespace {

constexpr std::array<std::string_view, 15> formatting_elements = {
    "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u", "mark"};

bool is_formatting(std::string_view name) {
  return std::ranges::find(formatting_elements, name) != formatting_elements.end();
}

// Start tags that close an open <p>.
bool closes_paragraph(std::string_view name) {
  static constexpr std::array<std::string_view, 40> names = {
      "address", "article", "aside",   "blockquote", "center", "details", "dialog", "dir",     "div",  "dl",
      "fieldset", "figcaption", "figure", "footer", "form",  "h1",      "h2",     "h3",      "h4",   "h5",
      "h6",      "header",  "hgroup",  "hr",         "main",   "menu",    "nav",    "ol",      "p",    "pre",
      "section", "summary", "table",   "ul",         "listing", "xmp",    "plaintext", "dd", "dt", "li"};
  return std::ranges::find(names, name) != names.end();
}

bool is_heading(std::string_view name) {
  return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

bool is_table_section(std::string_view name) { return name == "tbody" || name == "thead" || name == "tfoot"; }
bool is_table_context(std::string_view name) { return name == "table" || is_table_section(name) || name == "tr"; }
bool is_cell(std::string_view name) { return name == "td" || name == "th"; }

// Elements whose content is not markup.
enum class TextMode { Data, RawText, RcData, Plaintext };

TextMode text_mode_for(std::string_view name) {
  if (name == "script" || name == "style" || name == "xmp" || name == "iframe" || name == "noembed" ||
      name == "noframes") {
    return TextMode::RawText;
  }
  if (name == "title" || name == "textarea") return TextMode::RcData;
  if (name == "plaintext") return TextMode::Plaintext;
  return TextMode::Data;
}

// Contents of these are skipped entirely: never part of the document text.
bool is_skipped(std::string_view name) {
  return name == "script" || name == "style" || name == "template" || name == "noembed" || name == "noframes" ||
         name == "iframe";
}

struct Token {
  enum class Kind { StartTag, EndTag, Text, Comment, Doctype, Eof } kind = Kind::Eof;
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::string text;
  bool self_closing = false;
};

class Tokenizer {
 public:
  Tokenizer(std::string_view input, bool xhtml) : input_(input), xhtml_(xhtml) {}

  void set_mode(TextMode mode, std::string end_name) {
    mode_ = mode;
    raw_end_ = std::move(end_name);
  }
  void set_cdata_allowed(bool allowed) { cdata_allowed_ = allowed; }

  Token next() {
    Token token;
    if (pos_ >= input_.size()) return token;
    if (mode_ != TextMode::Data) return raw_text();
    if (input_[pos_] == '<') {
      if (auto tag = markup()) return std::move(*tag);
    }
    // Text up to the next '<' that starts markup.
    const std::size_t start = pos_;
    std::size_t i = pos_ + (input_[pos_] == '<' ? 1 : 0);
    while (i < input_.size()) {
      const std::size_t lt = input_.find('<', i);
      if (lt == std::string_view::npos) {
        i = input_.size();
        break;
      }
      if (lt + 1 < input_.size()) {
        const char n = input_[lt + 1];
        if (util::is_ascii_alpha(n) || n == '/' || n == '!' || n == '?') {
          i = lt;
          break;
        }
      }
      i = lt + 1;
    }
    pos_ = i;
    token.kind = Token::Kind::Text;
    token.text = decode_references(input_.substr(start, i - start), false);
    return token;
  }

 private:
  Token raw_text() {
    Token token;
    token.kind = Token::Kind::Text;
    if (mode_ == TextMode::Plaintext) {
      token.text = std::string(input_.substr(pos_));
      pos_ = input_.size();
      return token;
    }
    // Find "</name" followed by whitespace, '/' or '>' (case-insensitive).
    std::size_t search = pos_;
    std::size_t end = input_.size();
    while (search < input_.size()) {
      const std::size_t lt = input_.find("</", search);
      if (lt == std::string_view::npos) break;
      const std::string_view rest = input_.substr(lt + 2);
      if (util::istarts_with(rest, raw_end_)) {
        const std::size_t after = lt + 2 + raw_end_.size();
        if (after >= input_.size() || util::is_ascii_space(input_[after]) || input_[after] == '/' || input_[after] == '>') {
          end = lt;
          break;
        }
      }
      search = lt + 2;
    }
    const std::string_view content = input_.substr(pos_, end - pos_);
    token.text = mode_ == TextMode::RcData ? decode_references(content, false) : std::string(content);
    pos_ = end;
    mode_ = TextMode::Data;
    if (token.text.empty()) return next();
    return token;
  }

  std::optional<Token> markup() {
    const std::string_view rest = input_.substr(pos_);
    if (rest.size() < 2) return std::nullopt;
    const char n = rest[1];
    if (util::is_ascii_alpha(n)) return tag(false);
    if (n == '/') {
      if (rest.size() >= 3 && util::is_ascii_alpha(rest[2])) return tag(true);
      if (rest.size() >= 3 && rest[2] == '>') {
        pos_ += 3;
        return next();
      }
      return bogus_comment(2);
    }
    if (n == '!') {
      if (rest.starts_with("<!--")) return comment();
      if (rest.starts_with("<![CDATA[")) {
        const std::size_t end = input_.find("]]>", pos_ + 9);
        Token token;
        token.kind = cdata_allowed_ || xhtml_ ? Token::Kind::Text : Token::Kind::Comment;
        token.text = std::string(input_.substr(pos_ + 9, (end == std::string_view::npos ? input_.size() : end) - pos_ - 9));
        pos_ = end == std::string_view::npos ? input_.size() : end + 3;
        return token;
      }
      if (util::istarts_with(rest, "<!doctype")) {
        const std::size_t end = input_.find('>', pos_);
        Token token;
        token.kind = Token::Kind::Doctype;
        pos_ = end == std::string_view::npos ? input_.size() : end + 1;
        return token;
      }
      return bogus_comment(2);
    }
    if (n == '?') return bogus_comment(1);
    return std::nullopt;
  }

  Token bogus_comment(std::size_t skip) {
    Token token;
    token.kind = Token::Kind::Comment;
    const std::size_t end = input_.find('>', pos_ + skip);
    token.text = std::string(input_.substr(pos_ + skip, (end == std::string_view::npos ? input_.size() : end) - pos_ - skip));
    pos_ = end == std::string_view::npos ? input_.size() : end + 1;
    return token;
  }

  Token comment() {
    Token token;
    token.kind = Token::Kind::Comment;
    const std::size_t start = pos_ + 4;
    // "<!-->" and "<!--->" are complete (empty) comments.
    if (input_.substr(start).starts_with(">")) {
      pos_ = start + 1;
      return token;
    }
    if (input_.substr(start).starts_with("->")) {
      pos_ = start + 2;
      return token;
    }
    std::size_t end = input_.find("--", start);
    while (end != std::string_view::npos) {
      if (input_.substr(end).starts_with("-->")) {
        token.text = std::string(input_.substr(start, end - start));
        pos_ = end + 3;
        return token;
      }
      if (input_.substr(end).starts_with("--!>")) {
        token.text = std::string(input_.substr(start, end - start));
        pos_ = end + 4;
        return token;
      }
      end = input_.find("--", end + 1);
    }
    token.text = std::string(input_.substr(start));
    pos_ = input_.size();
    return token;
  }

  Token tag(bool end_tag) {
    Token token;
    token.kind = end_tag ? Token::Kind::EndTag : Token::Kind::StartTag;
    std::size_t i = pos_ + (end_tag ? 2 : 1);
    const std::size_t name_start = i;
    while (i < input_.size() && !util::is_ascii_space(input_[i]) && input_[i] != '/' && input_[i] != '>') ++i;
    token.name = util::to_lower(input_.substr(name_start, i - name_start));
    // Attributes.
    while (i < input_.size()) {
      while (i < input_.size() && (util::is_ascii_space(input_[i]) || input_[i] == '/')) {
        if (input_[i] == '/' && i + 1 < input_.size() && input_[i + 1] == '>') {
          token.self_closing = true;
        }
        ++i;
      }
      if (i >= input_.size()) break;
      if (input_[i] == '>') {
        ++i;
        pos_ = i;
        return token;
      }
      const std::size_t attr_start = i;
      ++i;  // the first character may be '='
      while (i < input_.size() && !util::is_ascii_space(input_[i]) && input_[i] != '/' && input_[i] != '>' &&
             input_[i] != '=') {
        ++i;
      }
      std::string name = util::to_lower(input_.substr(attr_start, i - attr_start));
      std::size_t j = i;
      while (j < input_.size() && util::is_ascii_space(input_[j])) ++j;
      std::string value;
      if (j < input_.size() && input_[j] == '=') {
        ++j;
        while (j < input_.size() && util::is_ascii_space(input_[j])) ++j;
        if (j < input_.size() && (input_[j] == '"' || input_[j] == '\'')) {
          const char quote = input_[j];
          const std::size_t close = input_.find(quote, j + 1);
          const std::size_t stop = close == std::string_view::npos ? input_.size() : close;
          value = decode_references(input_.substr(j + 1, stop - j - 1), true);
          j = close == std::string_view::npos ? input_.size() : close + 1;
        } else {
          const std::size_t value_start = j;
          while (j < input_.size() && !util::is_ascii_space(input_[j]) && input_[j] != '>') ++j;
          value = decode_references(input_.substr(value_start, j - value_start), true);
        }
        i = j;
      }
      if (!end_tag && token.attributes.size() < 256 &&
          std::ranges::none_of(token.attributes, [&](const auto& a) { return a.first == name; })) {
        token.attributes.emplace_back(std::move(name), std::move(value));
      }
    }
    pos_ = input_.size();
    return token;  // EOF inside a tag: the tag is still emitted
  }

  std::string_view input_;
  bool xhtml_ = false;
  std::size_t pos_ = 0;
  TextMode mode_ = TextMode::Data;
  std::string raw_end_;
  bool cdata_allowed_ = false;
};

class TreeBuilder {
 public:
  TreeBuilder(Dom& dom, const ParseSettings& settings) : dom_(dom), settings_(settings) {
    Node document;
    document.type = NodeType::Document;
    dom_.nodes.push_back(std::move(document));
    stack_.push_back(0);
  }

  void run(std::string_view input) {
    Tokenizer tokenizer(input, settings_.xhtml);
    std::size_t skip_depth = 0;  // inside <script>/<style>/<template>
    std::string skip_name;
    while (true) {
      tokenizer.set_cdata_allowed(in_foreign());
      Token token = tokenizer.next();
      if (token.kind == Token::Kind::Eof) break;
      if (skip_depth > 0) {
        if (skip_name == "style" && token.kind == Token::Kind::Text && stylesheet_.size() < 256 * 1024) {
          stylesheet_.append(token.text).push_back('\n');
        }
        if (token.kind == Token::Kind::StartTag && token.name == skip_name) ++skip_depth;
        if (token.kind == Token::Kind::EndTag && token.name == skip_name) --skip_depth;
        if (skip_depth > 0 && token.kind == Token::Kind::StartTag && !token.self_closing) {
          const TextMode mode = text_mode_for(token.name);
          if (mode != TextMode::Data) tokenizer.set_mode(mode, token.name);
        }
        continue;
      }
      switch (token.kind) {
        case Token::Kind::Text: text(std::move(token.text)); break;
        case Token::Kind::Comment: break;
        case Token::Kind::Doctype: break;
        case Token::Kind::StartTag: {
          if (is_skipped(token.name) && !in_foreign()) {
            if (token.self_closing && settings_.xhtml) break;
            skip_depth = 1;
            skip_name = token.name;
            const TextMode mode = text_mode_for(token.name);
            if (mode != TextMode::Data) tokenizer.set_mode(mode, token.name);
            break;
          }
          const bool pushed = start_tag(std::move(token));
          if (pushed && !in_foreign()) {
            const std::string& name = dom_.nodes[stack_.back()].name;
            const TextMode mode = text_mode_for(name);
            if (mode != TextMode::Data) tokenizer.set_mode(mode, name);
          }
          break;
        }
        case Token::Kind::EndTag: end_tag(token.name); break;
        case Token::Kind::Eof: break;
      }
      if (dom_.nodes.size() >= settings_.max_nodes) {
        dom_.warnings.emplace_back("The page has too many elements; the rest was not read.");
        break;
      }
    }
  }

 private:
  [[nodiscard]] NodeId current() const { return stack_.back(); }
  [[nodiscard]] const std::string& name_of(NodeId id) const { return dom_.nodes[id].name; }

  bool in_foreign() const {
    if (open_foreign_ == 0) return false;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      const std::string& n = name_of(*it);
      if (n == "svg" || n == "math") return true;
      if (n == "foreignobject" || n == "desc") return false;
    }
    return false;
  }

  // The stack of open elements is changed only through these two, which keep
  // counts that let the frequent scope checks skip a scan of the stack.
  void push_open(NodeId id) {
    stack_.push_back(id);
    count_open(id, 1);
  }

  void pop_open() {
    count_open(stack_.back(), -1);
    stack_.pop_back();
  }

  void count_open(NodeId id, int delta) {
    const std::string& n = name_of(id);
    if (n == "p") open_paragraphs_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(open_paragraphs_) + delta);
    if (n == "svg" || n == "math") open_foreign_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(open_foreign_) + delta);
  }

  NodeId new_node(NodeType type) {
    dom_.nodes.emplace_back();
    dom_.nodes.back().type = type;
    return static_cast<NodeId>(dom_.nodes.size() - 1);
  }

  // Where new content goes: the current node, or before the table when
  // the current node is table structure (foster parenting).
  void insert(NodeId child, bool foster_allowed = true) {
    NodeId parent = current();
    if (foster_allowed && is_table_context(name_of(parent))) {
      // Find the table on the stack and insert before it in its parent.
      for (std::size_t k = stack_.size(); k-- > 0;) {
        if (name_of(stack_[k]) == "table") {
          const NodeId table = stack_[k];
          const NodeId table_parent = dom_.nodes[table].parent;
          if (table_parent == no_node) break;
          auto& siblings = dom_.nodes[table_parent].children;
          const auto at = std::ranges::find(siblings, table);
          dom_.nodes[child].parent = table_parent;
          siblings.insert(at, child);
          return;
        }
      }
    }
    dom_.nodes[child].parent = parent;
    dom_.nodes[parent].children.push_back(child);
  }

  void text(std::string value) {
    if (value.empty()) return;
    const bool whitespace = std::ranges::all_of(value, [](char c) { return util::is_ascii_space(c); });
    const std::string& cur = name_of(current());
    if (is_table_context(cur)) {
      if (whitespace) return;  // layout whitespace between table parts
    }
    if (!whitespace) reconstruct_formatting();
    // Merge with a preceding text node at the insertion point.
    NodeId parent = current();
    if (!is_table_context(name_of(parent))) {
      auto& children = dom_.nodes[parent].children;
      if (!children.empty() && dom_.nodes[children.back()].type == NodeType::Text) {
        dom_.nodes[children.back()].text.append(value);
        return;
      }
    }
    const NodeId id = new_node(NodeType::Text);
    dom_.nodes[id].text = std::move(value);
    insert(id);
  }

  bool has_in_scope(std::string_view name, bool list_scope = false, bool button_scope = false) const {
    if (name == "p" && open_paragraphs_ == 0) return false;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      if (*it == 0) return false;
      const std::string& n = name_of(*it);
      if (n == name) return true;
      if (n == "table" || is_cell(n) || n == "caption" || n == "marquee" || n == "object" || n == "applet" ||
          n == "template" || n == "svg" || n == "math") {
        return false;
      }
      if (list_scope && (n == "ol" || n == "ul")) return false;
      if (button_scope && n == "button") return false;
    }
    return false;
  }

  void pop_until(std::string_view name) {
    while (stack_.size() > 1) {
      const bool match = name_of(current()) == name;
      pop_open();
      if (match) return;
    }
  }

  void close_paragraph() {
    if (has_in_scope("p", false, true)) pop_until("p");
  }

  void close_cell() {
    for (std::size_t k = stack_.size(); k-- > 1;) {
      const std::string& n = name_of(stack_[k]);
      if (is_cell(n)) {
        while (stack_.size() > k) pop_open();
        clear_formatting_to_marker();
        return;
      }
      if (n == "table") return;
    }
  }

  bool in_table_scope(std::string_view name) const {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      const std::string& n = name_of(*it);
      if (n == name) return true;
      if (n == "table" || *it == 0) return false;
    }
    return false;
  }

  void pop_to_table_context(std::initializer_list<std::string_view> names) {
    while (stack_.size() > 1) {
      const std::string& n = name_of(current());
      if (std::ranges::find(names, n) != names.end() || n == "table") return;
      pop_open();
    }
  }

  NodeId push_element(const std::string& name, std::vector<std::pair<std::string, std::string>> attributes) {
    const NodeId id = new_node(NodeType::Element);
    dom_.nodes[id].name = name;
    dom_.nodes[id].attributes = std::move(attributes);
    dom_.nodes[id].foreign = in_foreign() || name == "svg" || name == "math";
    // Table structure goes into the table; anything else is fostered out of it.
    const bool structure = is_table_context(name) || is_cell(name) || name == "caption" || name == "colgroup" ||
                           name == "col" || name == "form" || name == "script" || name == "style" || name == "template";
    insert(id, !structure);
    if (stack_.size() < settings_.max_open_elements) {
      push_open(id);
    } else if (!depth_warned_) {
      depth_warned_ = true;
      dom_.warnings.emplace_back("The page is nested too deeply; deeper elements were flattened.");
    }
    return id;
  }

  // ---- active formatting elements -------------------------------------------

  void clear_formatting_to_marker() {
    while (!formatting_.empty()) {
      const NodeId last = formatting_.back();
      formatting_.pop_back();
      if (last == no_node) return;
    }
  }

  void reconstruct_formatting() {
    if (formatting_.empty()) return;
    // Find the first entry (after the last marker) that is not open.
    std::size_t first = formatting_.size();
    for (std::size_t k = formatting_.size(); k-- > 0;) {
      const NodeId entry = formatting_[k];
      if (entry == no_node) break;
      if (std::ranges::find(stack_, entry) != stack_.end()) break;
      first = k;
    }
    for (std::size_t k = first; k < formatting_.size(); ++k) {
      const Node& original = dom_.nodes[formatting_[k]];
      const std::string name = original.name;
      auto attributes = original.attributes;
      const NodeId clone = push_element(name, std::move(attributes));
      formatting_[k] = clone;
    }
  }

  void push_formatting(NodeId id) {
    if (formatting_.size() >= 64) return;
    // Noah's ark: at most three identical entries after the last marker.
    int same = 0;
    for (std::size_t k = formatting_.size(); k-- > 0;) {
      if (formatting_[k] == no_node) break;
      const Node& n = dom_.nodes[formatting_[k]];
      if (n.name == dom_.nodes[id].name && n.attributes == dom_.nodes[id].attributes && ++same >= 3) {
        formatting_.erase(formatting_.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
    }
    formatting_.push_back(id);
  }

  // ---- tags -------------------------------------------------------------------

  bool start_tag(Token token) {
    const std::string& name = token.name;
    if (in_foreign()) {
      // SVG / MathML: generic XML-like nesting; self-closing honoured.
      push_element(name, std::move(token.attributes));
      if (token.self_closing) {
        pop_open();
        return false;
      }
      return true;
    }
    if (name == "html" || name == "body" || name == "head") {
      // Structure is implied; keep attributes (lang, dir) on the document node.
      for (auto& attribute : token.attributes) {
        if (!dom_.has_attribute(0, attribute.first)) dom_.nodes[0].attributes.push_back(std::move(attribute));
      }
      return false;
    }
    if (closes_paragraph(name)) close_paragraph();
    if (is_heading(name) && is_heading(name_of(current()))) pop_open();
    if (name == "li") close_list_item({"li"});
    if (name == "dd" || name == "dt") close_list_item({"dd", "dt"});
    if (name == "option" && name_of(current()) == "option") pop_open();
    if (name == "a") {
      // An <a> inside an open <a> closes the first one.
      for (std::size_t k = formatting_.size(); k-- > 0;) {
        if (formatting_[k] == no_node) break;
        if (name_of(formatting_[k]) == "a") {
          end_tag("a");
          break;
        }
      }
    }
    if (is_cell(name)) {
      if (!in_table_scope("table")) return ignore_table_part(std::move(token));
      close_cell();
      if (name_of(current()) != "tr") {
        pop_to_table_context({"tbody", "thead", "tfoot", "tr"});
        if (name_of(current()) == "table") push_element("tbody", {});
        if (name_of(current()) != "tr") push_element("tr", {});
      }
      push_element(name, std::move(token.attributes));
      formatting_.push_back(no_node);  // marker
      return true;
    }
    if (name == "tr") {
      if (!in_table_scope("table")) return ignore_table_part(std::move(token));
      close_cell();
      pop_to_table_context({"tbody", "thead", "tfoot"});
      if (name_of(current()) == "table") push_element("tbody", {});
      push_element(name, std::move(token.attributes));
      return true;
    }
    if (is_table_section(name) || name == "caption" || name == "colgroup" || name == "col") {
      if (!in_table_scope("table")) return ignore_table_part(std::move(token));
      close_cell();
      pop_to_table_context({});
      push_element(name, std::move(token.attributes));
      if (name == "col" || token.self_closing) {
        pop_open();
        return false;
      }
      if (name == "caption") formatting_.push_back(no_node);
      return true;
    }
    if (name == "table" && name_of(current()) == "table") pop_open();
    if (name == "image") {
      token.name = "img";
      return start_tag(std::move(token));
    }
    const bool formatting = is_formatting(name);
    if (!closes_paragraph(name) || formatting) reconstruct_formatting();
    const NodeId id = push_element(name, std::move(token.attributes));
    if (is_void_element(name) || (token.self_closing && settings_.xhtml)) {
      if (stack_.back() == id) pop_open();
      return false;
    }
    if (formatting && stack_.back() == id) push_formatting(id);
    if (name == "table") formatting_.push_back(no_node);
    return stack_.back() == id;
  }

  // A table part outside a table is ignored (its content still flows in).
  bool ignore_table_part(Token token) {
    (void)token;
    return false;
  }

  void close_list_item(std::initializer_list<std::string_view> names) {
    for (std::size_t k = stack_.size(); k-- > 1;) {
      const std::string& n = name_of(stack_[k]);
      if (std::ranges::find(names, n) != names.end()) {
        while (stack_.size() > k) pop_open();
        return;
      }
      // Stop at anything but phrasing/formatting wrappers.
      if (n == "ul" || n == "ol" || n == "dl" || n == "table" || n == "menu" || is_cell(n)) return;
      if (n != "p" && n != "div" && n != "address" && !is_formatting(n) && n != "span") return;
    }
  }

  void end_tag(const std::string& name) {
    if (name == "html" || name == "body" || name == "head") return;
    if (name == "br") {
      Token token;
      token.name = "br";
      start_tag(std::move(token));
      return;
    }
    if (in_foreign()) {
      for (std::size_t k = stack_.size(); k-- > 1;) {
        if (name_of(stack_[k]) == name) {
          while (stack_.size() > k) pop_open();
          return;
        }
        if (!dom_.nodes[stack_[k]].foreign) break;
      }
    }
    if (name == "p") {
      if (has_in_scope("p", false, true)) pop_until("p");
      return;
    }
    if (is_cell(name)) {
      if (in_table_scope(name)) close_cell();
      return;
    }
    if (name == "tr" || is_table_section(name) || name == "table" || name == "caption") {
      if (!in_table_scope(name)) return;
      if (name == "table" || name == "caption") close_cell();
      while (stack_.size() > 1) {
        const bool match = name_of(current()) == name;
        pop_open();
        if (match) break;
      }
      if (name == "table" || name == "caption") clear_formatting_to_marker();
      return;
    }
    if (is_formatting(name)) {
      formatting_end(name);
      return;
    }
    // Any other element: close it when open (not across table boundaries).
    for (std::size_t k = stack_.size(); k-- > 1;) {
      const std::string& n = name_of(stack_[k]);
      if (n == name || (is_heading(name) && is_heading(n))) {
        while (stack_.size() > k) pop_open();
        return;
      }
      if (n == "table" || is_cell(n) || n == "caption") return;
    }
  }

  // Simplified adoption agency: close the formatting element and re-open the
  // elements that were open inside it, so text keeps its order and the
  // misnested formatting applies exactly where it was in effect.
  void formatting_end(const std::string& name) {
    std::size_t entry = formatting_.size();
    for (std::size_t k = formatting_.size(); k-- > 0;) {
      if (formatting_[k] == no_node) break;
      if (name_of(formatting_[k]) == name) {
        entry = k;
        break;
      }
    }
    if (entry == formatting_.size()) {
      // Not active: treat like any other end tag.
      for (std::size_t k = stack_.size(); k-- > 1;) {
        if (name_of(stack_[k]) == name) {
          while (stack_.size() > k) pop_open();
          return;
        }
        if (name_of(stack_[k]) == "table" || is_cell(name_of(stack_[k]))) return;
      }
      return;
    }
    const NodeId element = formatting_[entry];
    const auto on_stack = std::ranges::find(stack_, element);
    formatting_.erase(formatting_.begin() + static_cast<std::ptrdiff_t>(entry));
    if (on_stack == stack_.end()) return;  // already closed implicitly
    const auto index = static_cast<std::size_t>(on_stack - stack_.begin());
    // Do not cross a table cell boundary.
    for (std::size_t k = index + 1; k < stack_.size(); ++k) {
      if (is_table_context(name_of(stack_[k])) || is_cell(name_of(stack_[k]))) return;
    }
    std::vector<NodeId> reopened(stack_.begin() + static_cast<std::ptrdiff_t>(index) + 1, stack_.end());
    while (stack_.size() > index) pop_open();
    for (const NodeId id : reopened) {
      const Node& original = dom_.nodes[id];
      const std::string clone_name = original.name;
      auto attributes = original.attributes;
      const NodeId clone = push_element(clone_name, std::move(attributes));
      // Keep formatting entries pointing at the open clone.
      for (NodeId& f : formatting_) {
        if (f == id) f = clone;
      }
    }
  }

  Dom& dom_;
  const ParseSettings& settings_;

 public:
  std::string stylesheet_;

 private:
  std::vector<NodeId> stack_;
  std::vector<NodeId> formatting_;  // no_node = marker
  bool depth_warned_ = false;
  std::size_t open_paragraphs_ = 0;
  std::size_t open_foreign_ = 0;
};

}  // namespace

Dom parse_dom(std::string_view utf8, const ParseSettings& settings) {
  Dom dom;
  dom.nodes.reserve(std::min<std::size_t>(utf8.size() / 8 + 16, 1u << 20));
  TreeBuilder builder(dom, settings);
  builder.run(utf8);
  // Class rules of the stylesheet.
  const std::string_view css = builder.stylesheet_;
  std::size_t i = 0;
  while (i < css.size() && dom.class_rules.size() < 4096) {
    const std::size_t open = css.find('{', i);
    if (open == std::string_view::npos) break;
    const std::size_t close = css.find('}', open);
    if (close == std::string_view::npos) break;
    const std::string body = util::to_lower(css.substr(open + 1, close - open - 1));
    for (const std::string_view selector : util::split(css.substr(i, open - i), ',', false)) {
      const std::string_view s = util::trim(selector);
      const std::size_t dot = s.find('.');
      if (dot == std::string_view::npos || s.find(' ') != std::string_view::npos || s.find(':') != std::string_view::npos) continue;
      dom.class_rules[std::string(s.substr(dot + 1))] += body + ";";
    }
    i = close + 1;
  }
  return dom;
}

}  // namespace cvt::document::html
