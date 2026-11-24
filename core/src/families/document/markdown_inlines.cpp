// Convertible core - document family: Markdown inline parsing.
//
// CommonMark inline algorithm: code spans, backslash escapes, entity and
// numeric references, autolinks, raw HTML, hard and soft breaks, links and
// images (inline, full/collapsed/shortcut reference) with the bracket stack,
// and emphasis with the delimiter-run rules ("process emphasis", including
// the rule of 3). GFM: strikethrough, extended autolinks, footnote
// references; extended flavour: $math$ and $$display math$$. Simple inline
// HTML pairs (<u>, <sup>, <sub>, <del>, <mark>, <kbd>) and <br> become
// formatting nodes instead of raw HTML so that they survive conversions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/html_internal.hpp"
#include "families/document/markdown_internal.hpp"
#include "util/text.hpp"

namespace cvt::document::markdown {

// =========================================================================== shared helpers

std::string normalize_label(std::string_view label) {
  std::string out;
  bool space = false;
  std::size_t i = 0;
  const std::string_view t = util::trim(label);
  while (i < t.size()) {
    const util::Utf8Decoded d = util::decode_utf8(t, i);
    i += std::max<std::size_t>(d.length, 1);
    char32_t cp = d.code_point;
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3A9) || (cp >= 0x410 && cp <= 0x42F)) {
      cp += 0x20;
    } else if (cp >= 0x400 && cp <= 0x40F) {
      cp += 0x50;
    } else if (cp == 0x1E9E || cp == 0xDF) {
      out.append("ss");
      continue;
    }
    util::append_utf8(out, cp);
  }
  return out;
}

std::size_t parse_entity(std::string_view text, std::size_t pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '&') return 0;
  std::size_t i = pos + 1;
  if (i < text.size() && text[i] == '#') {
    ++i;
    bool hex = false;
    if (i < text.size() && (text[i] == 'x' || text[i] == 'X')) {
      hex = true;
      ++i;
    }
    const std::size_t start = i;
    std::uint32_t value = 0;
    while (i < text.size() && i - start < (hex ? 6u : 7u)) {
      const char c = text[i];
      int digit = -1;
      if (util::is_ascii_digit(c)) digit = c - '0';
      if (hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      if (hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      if (digit < 0) break;
      value = value * (hex ? 16u : 10u) + static_cast<std::uint32_t>(digit);
      ++i;
    }
    if (i == start || i >= text.size() || text[i] != ';') return 0;
    char32_t cp = value;
    if (cp == 0 || !util::is_scalar_value(cp)) cp = 0xFFFD;
    util::append_utf8(out, cp);
    return i + 1 - pos;
  }
  const std::size_t start = i;
  while (i < text.size() && i - start < 32 && (util::is_ascii_alpha(text[i]) || util::is_ascii_digit(text[i]))) ++i;
  if (i == start || i >= text.size() || text[i] != ';') return 0;
  const std::string name = std::string(text.substr(start, i - start)) + ";";
  const std::string_view value = html::lookup_named_reference(name);
  if (value.empty()) return 0;
  out.append(value);
  return i + 1 - pos;
}

std::string unescape_string(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() && is_ascii_punctuation(text[i + 1])) {
      out.push_back(text[i + 1]);
      ++i;
      continue;
    }
    if (text[i] == '&') {
      const std::size_t used = parse_entity(text, i, out);
      if (used > 0) {
        i += used - 1;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

ir::ImageSource image_source(std::string_view url, const ParseState& state) {
  if (auto embedded = parse_data_uri_image(url)) return std::move(*embedded);
  if (state.options != nullptr && state.options->resolve) {
    if (auto resolved = state.options->resolve(url)) return std::move(*resolved);
  }
  ir::EncodedImage reference;
  reference.url = std::string(url);
  return reference;
}

// ---- raw HTML ------------------------------------------------------------------------

namespace {

bool tag_name_char(char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-'; }

std::size_t skip_spaces(std::string_view s, std::size_t i, bool& had_newline_or_space) {
  const std::size_t start = i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  had_newline_or_space = i > start;
  return i;
}

}  // namespace

std::size_t match_open_tag(std::string_view s, std::size_t pos) {
  std::size_t i = pos;
  if (i >= s.size() || s[i] != '<') return 0;
  ++i;
  if (i >= s.size() || !util::is_ascii_alpha(s[i])) return 0;
  while (i < s.size() && tag_name_char(s[i])) ++i;
  while (true) {
    bool spaced = false;
    const std::size_t after_space = skip_spaces(s, i, spaced);
    if (after_space >= s.size()) return 0;
    if (s[after_space] == '>') return after_space + 1 - pos;
    if (s.substr(after_space).starts_with("/>")) return after_space + 2 - pos;
    if (!spaced) return 0;
    // Attribute name.
    i = after_space;
    if (!(util::is_ascii_alpha(s[i]) || s[i] == '_' || s[i] == ':')) return 0;
    ++i;
    while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]) || s[i] == '_' || s[i] == '.' ||
                            s[i] == ':' || s[i] == '-')) {
      ++i;
    }
    // Optional value.
    bool sp = false;
    std::size_t j = skip_spaces(s, i, sp);
    if (j < s.size() && s[j] == '=') {
      j = skip_spaces(s, j + 1, sp);
      if (j >= s.size()) return 0;
      if (s[j] == '"' || s[j] == '\'') {
        const std::size_t close = s.find(s[j], j + 1);
        if (close == std::string_view::npos) return 0;
        i = close + 1;
      } else {
        const std::size_t start = j;
        while (j < s.size() && !util::is_ascii_space(s[j]) && s[j] != '"' && s[j] != '\'' && s[j] != '=' &&
               s[j] != '<' && s[j] != '>' && s[j] != '`') {
          ++j;
        }
        if (j == start) return 0;
        i = j;
      }
    }
  }
}

std::size_t match_close_tag(std::string_view s, std::size_t pos) {
  std::size_t i = pos;
  if (!s.substr(i).starts_with("</")) return 0;
  i += 2;
  if (i >= s.size() || !util::is_ascii_alpha(s[i])) return 0;
  while (i < s.size() && tag_name_char(s[i])) ++i;
  bool sp = false;
  i = skip_spaces(s, i, sp);
  if (i >= s.size() || s[i] != '>') return 0;
  return i + 1 - pos;
}

std::size_t match_raw_html(std::string_view s, std::size_t pos) {
  if (std::size_t n = match_open_tag(s, pos)) return n;
  if (std::size_t n = match_close_tag(s, pos)) return n;
  const std::string_view rest = s.substr(pos);
  if (rest.starts_with("<!-->")) return 5;
  if (rest.starts_with("<!--->")) return 6;
  if (rest.starts_with("<!--")) {
    const std::size_t end = rest.find("-->", 4);
    return end == std::string_view::npos ? 0 : end + 3;
  }
  if (rest.starts_with("<?")) {
    const std::size_t end = rest.find("?>", 2);
    return end == std::string_view::npos ? 0 : end + 2;
  }
  if (rest.starts_with("<![CDATA[")) {
    const std::size_t end = rest.find("]]>", 9);
    return end == std::string_view::npos ? 0 : end + 3;
  }
  if (rest.size() > 2 && rest.starts_with("<!") && util::is_ascii_alpha(rest[2])) {
    const std::size_t end = rest.find('>', 2);
    return end == std::string_view::npos ? 0 : end + 1;
  }
  return 0;
}

// =========================================================================== link reference definitions

namespace {

// Link destination at s[i]: <...> or a balanced-parentheses run. Returns the
// end index (exclusive) and the raw destination, or npos.
std::size_t parse_destination(std::string_view s, std::size_t i, std::string& destination) {
  if (i < s.size() && s[i] == '<') {
    std::size_t j = i + 1;
    while (j < s.size()) {
      if (s[j] == '\\' && j + 1 < s.size() && is_ascii_punctuation(s[j + 1])) {
        j += 2;
        continue;
      }
      if (s[j] == '>') {
        destination = unescape_string(s.substr(i + 1, j - i - 1));
        return j + 1;
      }
      if (s[j] == '<' || s[j] == '\n' || s[j] == '\r') return std::string_view::npos;
      ++j;
    }
    return std::string_view::npos;
  }
  std::size_t j = i;
  int depth = 0;
  while (j < s.size()) {
    const char c = s[j];
    if (c == '\\' && j + 1 < s.size() && is_ascii_punctuation(s[j + 1])) {
      j += 2;
      continue;
    }
    if (c == '(') {
      if (++depth > 32) return std::string_view::npos;
    } else if (c == ')') {
      if (depth == 0) break;
      --depth;
    } else if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
      break;
    }
    ++j;
  }
  if (j == i || depth != 0) return std::string_view::npos;
  destination = unescape_string(s.substr(i, j - i));
  return j;
}

std::size_t parse_title(std::string_view s, std::size_t i, std::string& title) {
  if (i >= s.size()) return std::string_view::npos;
  const char open = s[i];
  char close = 0;
  if (open == '"' || open == '\'') close = open;
  else if (open == '(') close = ')';
  else return std::string_view::npos;
  std::size_t j = i + 1;
  while (j < s.size()) {
    if (s[j] == '\\' && j + 1 < s.size() && is_ascii_punctuation(s[j + 1])) {
      j += 2;
      continue;
    }
    if (s[j] == close) {
      title = unescape_string(s.substr(i + 1, j - i - 1));
      return j + 1;
    }
    if (open == '(' && s[j] == '(') return std::string_view::npos;
    // A blank line ends the title.
    if (s[j] == '\n') {
      std::size_t k = j + 1;
      while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
      if (k < s.size() && s[k] == '\n') return std::string_view::npos;
    }
    ++j;
  }
  return std::string_view::npos;
}

// Link label "[...]" at s[i]: returns end index (after ']') or npos.
std::size_t parse_label(std::string_view s, std::size_t i, std::string& label) {
  if (i >= s.size() || s[i] != '[') return std::string_view::npos;
  std::size_t j = i + 1;
  while (j < s.size() && j - i <= 1000) {
    if (s[j] == '\\' && j + 1 < s.size()) {
      j += 2;
      continue;
    }
    if (s[j] == '[') return std::string_view::npos;
    if (s[j] == ']') {
      label = std::string(s.substr(i + 1, j - i - 1));
      if (util::trim(label).empty()) return std::string_view::npos;
      return j + 1;
    }
    ++j;
  }
  return std::string_view::npos;
}

std::size_t skip_spaces_newline(std::string_view s, std::size_t i, bool& newline) {
  newline = false;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  if (i < s.size() && s[i] == '\n') {
    newline = true;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  }
  return i;
}

}  // namespace

std::size_t parse_reference_definition(std::string_view s, ParseState& state) {
  std::string label;
  std::size_t i = parse_label(s, 0, label);
  if (i == std::string_view::npos || i >= s.size() || s[i] != ':') return 0;
  if (label.starts_with("^") && state.gfm()) return 0;  // footnote syntax
  ++i;
  bool newline = false;
  i = skip_spaces_newline(s, i, newline);
  std::string destination;
  const std::size_t after_destination = parse_destination(s, i, destination);
  if (after_destination == std::string_view::npos) return 0;
  if (after_destination == i && !(i < s.size() && s[i] == '<')) return 0;
  const std::size_t before_title = after_destination;
  i = skip_spaces_newline(s, after_destination, newline);
  std::string title;
  std::size_t end = std::string_view::npos;
  if (i > before_title) {
    const std::size_t after_title = parse_title(s, i, title);
    if (after_title != std::string_view::npos) {
      std::size_t k = after_title;
      while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
      if (k >= s.size() || s[k] == '\n') end = k < s.size() ? k + 1 : k;
    }
  }
  if (end == std::string_view::npos) {
    // Without a (valid) title the destination must end the line.
    title.clear();
    std::size_t k = before_title;
    while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
    if (k < s.size() && s[k] != '\n') return 0;
    end = k < s.size() ? k + 1 : k;
  }
  const std::string key = normalize_label(label);
  if (key.empty()) return 0;
  state.references.emplace(key, LinkReference{std::move(destination), std::move(title)});
  return end;
}

// =========================================================================== the inline parser

namespace {

enum class NK : std::uint8_t {
  Text, SoftBreak, HardBreak, Code, Emph, Strong, Strike, Link, Image, RawHtml, Math, DisplayMath, Note, Underline,
  Sup, Sub, Mark,
};

struct INode {
  NK kind = NK::Text;
  std::string text;  // Text, Code, RawHtml, Math; destination for links
  std::string title;
  int first = -1, last = -1, prev = -1, next = -1, parent = -1;
  int height = 0;
};

struct Delimiter {
  int node = -1;
  char ch = 0;
  int count = 0;
  int original = 0;
  bool can_open = false;
  bool can_close = false;
  int prev = -1, next = -1;
};

struct Bracket {
  int node = -1;  // the "[" or "![" text node
  bool image = false;
  bool active = true;
  int previous_delimiter = -1;
  std::size_t index = 0;  // position after "["
  bool bracket_after = false;
};

constexpr int max_height = 40;

bool is_unicode_whitespace(char32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool is_unicode_punctuation(char32_t cp) {
  if (cp < 0x80) return is_ascii_punctuation(static_cast<char>(cp));
  return (cp >= 0x2010 && cp <= 0x2027) || (cp >= 0x2030 && cp <= 0x205E) || (cp >= 0x3001 && cp <= 0x3003) ||
         (cp >= 0x3008 && cp <= 0x3011) || cp == 0xA1 || cp == 0xA7 || cp == 0xAB || cp == 0xB6 || cp == 0xB7 ||
         cp == 0xBB || cp == 0xBF || (cp >= 0xFF01 && cp <= 0xFF0F) || cp == 0x37E || cp == 0x387 ||
         (cp >= 0x55A && cp <= 0x55F) || cp == 0x589 || cp == 0x5BE || cp == 0x5C0 || cp == 0x5C3 || cp == 0x5C6 ||
         cp == 0x5F3 || cp == 0x5F4 || cp == 0x609 || cp == 0x60A || cp == 0x60C || cp == 0x60D || cp == 0x61B ||
         cp == 0x61F || (cp >= 0x66A && cp <= 0x66D) || cp == 0x6D4 || (cp >= 0x20A0 && cp <= 0x20C0) ||
         (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x2200 && cp <= 0x22FF) || (cp >= 0x2E00 && cp <= 0x2E4F) ||
         cp == 0xA2 || cp == 0xA3 || cp == 0xA4 || cp == 0xA5 || cp == 0xA6 || cp == 0xA8 || cp == 0xA9 || cp == 0xAC ||
         cp == 0xAE || cp == 0xAF || cp == 0xB0 || cp == 0xB1 || cp == 0xB4 || cp == 0xB8 || cp == 0xD7 || cp == 0xF7;
}

class Inliner {
 public:
  Inliner(std::string_view text, ParseState& state) : s_(text), state_(state) {}

  ir::Inlines run() {
    root_ = new_node(NK::Text);
    while (pos_ < s_.size()) {
      if (!parse_one()) {
        append_text(std::string_view(s_).substr(pos_, 1));
        ++pos_;
      }
    }
    process_emphasis(-1);
    pair_html_tags(root_);
    return to_ir(root_, 0);
  }

 private:
  // ---- node arena ----
  int new_node(NK kind, std::string text = {}) {
    nodes_.push_back(INode{});
    nodes_.back().kind = kind;
    nodes_.back().text = std::move(text);
    return static_cast<int>(nodes_.size() - 1);
  }

  void append_child(int parent, int child) {
    INode& c = nodes_[static_cast<std::size_t>(child)];
    c.parent = parent;
    c.next = -1;
    c.prev = nodes_[static_cast<std::size_t>(parent)].last;
    if (c.prev >= 0) {
      nodes_[static_cast<std::size_t>(c.prev)].next = child;
    } else {
      nodes_[static_cast<std::size_t>(parent)].first = child;
    }
    nodes_[static_cast<std::size_t>(parent)].last = child;
  }

  void unlink(int node) {
    INode& n = nodes_[static_cast<std::size_t>(node)];
    if (n.prev >= 0) {
      nodes_[static_cast<std::size_t>(n.prev)].next = n.next;
    } else if (n.parent >= 0) {
      nodes_[static_cast<std::size_t>(n.parent)].first = n.next;
    }
    if (n.next >= 0) {
      nodes_[static_cast<std::size_t>(n.next)].prev = n.prev;
    } else if (n.parent >= 0) {
      nodes_[static_cast<std::size_t>(n.parent)].last = n.prev;
    }
    n.parent = n.prev = n.next = -1;
  }

  void insert_after(int node, int sibling) {
    unlink(sibling);
    INode& n = nodes_[static_cast<std::size_t>(node)];
    INode& s = nodes_[static_cast<std::size_t>(sibling)];
    s.next = n.next;
    s.prev = node;
    s.parent = n.parent;
    if (n.next >= 0) {
      nodes_[static_cast<std::size_t>(n.next)].prev = sibling;
    } else if (n.parent >= 0) {
      nodes_[static_cast<std::size_t>(n.parent)].last = sibling;
    }
    n.next = sibling;
  }

  // Move the siblings strictly between `from` and `to` into `container`
  // (appended at the end); returns the height they need.
  int move_between(int from, int to, int container) {
    int height = 0;
    int current = nodes_[static_cast<std::size_t>(from)].next;
    while (current >= 0 && current != to) {
      const int next = nodes_[static_cast<std::size_t>(current)].next;
      unlink(current);
      append_child(container, current);
      height = std::max(height, nodes_[static_cast<std::size_t>(current)].height);
      current = next;
    }
    return height;
  }

  int height_between(int from, int to) const {
    int height = 0;
    for (int current = nodes_[static_cast<std::size_t>(from)].next; current >= 0 && current != to;
         current = nodes_[static_cast<std::size_t>(current)].next) {
      height = std::max(height, nodes_[static_cast<std::size_t>(current)].height);
    }
    return height;
  }

  int append_text(std::string_view text) {
    const int last = nodes_[static_cast<std::size_t>(root_)].last;
    // Never merge into a delimiter or bracket text node.
    if (last >= 0 && nodes_[static_cast<std::size_t>(last)].kind == NK::Text && !special_.contains(last)) {
      nodes_[static_cast<std::size_t>(last)].text.append(text);
      return last;
    }
    const int node = new_node(NK::Text, std::string(text));
    append_child(root_, node);
    return node;
  }

  int append_special(std::string_view text) {
    const int node = new_node(NK::Text, std::string(text));
    append_child(root_, node);
    special_.insert(node);
    return node;
  }

  [[nodiscard]] char peek(std::size_t at) const { return at < s_.size() ? s_[at] : '\0'; }

  // ---- dispatch ----
  bool parse_one() {
    const char c = s_[pos_];
    switch (c) {
      case '\n': return parse_newline();
      case '\\': return parse_backslash();
      case '`': return parse_code_span();
      case '*':
      case '_': return parse_delimiters(c);
      case '~': return state_.gfm() ? parse_delimiters(c) : false;
      case '[': return parse_open_bracket();
      case '!': return parse_bang();
      case ']': return parse_close_bracket();
      case '<': return parse_autolink() || parse_html();
      case '&': return parse_entity_here();
      case '$': return state_.gfm() ? parse_math() : false;
      default: return parse_text();
    }
  }

  bool parse_text() {
    std::size_t end = pos_ + 1;
    while (end < s_.size()) {
      const char c = s_[end];
      if (c == '\n' || c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == '!' || c == ']' ||
          c == '<' || c == '&' || (c == '~' && state_.gfm()) || (c == '$' && state_.gfm())) {
        break;
      }
      ++end;
    }
    std::string_view piece = std::string_view(s_).substr(pos_, end - pos_);
    // Trailing spaces before a newline are handled by parse_newline.
    append_text(piece);
    pos_ = end;
    return true;
  }

  bool parse_newline() {
    ++pos_;
    // Remove trailing spaces from the preceding text; two or more mean a hard break.
    const int last = nodes_[static_cast<std::size_t>(root_)].last;
    bool hard = false;
    if (last >= 0 && nodes_[static_cast<std::size_t>(last)].kind == NK::Text && !special_.contains(last)) {
      std::string& text = nodes_[static_cast<std::size_t>(last)].text;
      std::size_t spaces = 0;
      while (spaces < text.size() && text[text.size() - 1 - spaces] == ' ') ++spaces;
      hard = spaces >= 2;
      text.erase(text.size() - spaces);
    }
    append_child(root_, new_node(hard ? NK::HardBreak : NK::SoftBreak));
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
    return true;
  }

  bool parse_backslash() {
    const char next = peek(pos_ + 1);
    if (next == '\n') {
      append_child(root_, new_node(NK::HardBreak));
      pos_ += 2;
      while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
      return true;
    }
    if (is_ascii_punctuation(next)) {
      append_text(std::string_view(&s_[pos_ + 1], 1));
      pos_ += 2;
      return true;
    }
    append_text("\\");
    ++pos_;
    return true;
  }

  bool parse_entity_here() {
    std::string value;
    const std::size_t used = parse_entity(s_, pos_, value);
    if (used == 0) return false;
    append_text(value);
    pos_ += used;
    return true;
  }

  bool parse_code_span() {
    std::size_t ticks = 0;
    while (pos_ + ticks < s_.size() && s_[pos_ + ticks] == '`') ++ticks;
    const std::size_t content_start = pos_ + ticks;
    std::size_t search = content_start;
    // Cached: no closer of this length after `no_closer_after_[ticks]`.
    if (const auto it = no_closer_.find(ticks); it != no_closer_.end() && content_start >= it->second) {
      append_text(std::string_view(s_).substr(pos_, ticks));
      pos_ += ticks;
      return true;
    }
    while (search < s_.size()) {
      const std::size_t found = s_.find('`', search);
      if (found == std::string_view::npos) break;
      std::size_t run = 0;
      while (found + run < s_.size() && s_[found + run] == '`') ++run;
      if (run == ticks) {
        std::string content(s_.substr(content_start, found - content_start));
        for (char& c : content) {
          if (c == '\n') c = ' ';
        }
        if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' &&
            content.find_first_not_of(' ') != std::string::npos) {
          content = content.substr(1, content.size() - 2);
        }
        append_child(root_, new_node(NK::Code, std::move(content)));
        pos_ = found + run;
        return true;
      }
      search = found + run;
    }
    no_closer_[ticks] = content_start;
    append_text(std::string_view(s_).substr(pos_, ticks));
    pos_ += ticks;
    return true;
  }

  bool parse_math() {
    // $$display$$ or $inline$ (no space after the opener / before the closer,
    // and the closer not followed by a digit).
    const bool display = peek(pos_ + 1) == '$';
    const std::size_t open_len = display ? 2 : 1;
    const std::size_t start = pos_ + open_len;
    if (start >= s_.size()) return false;
    if (!display && (s_[start] == ' ' || s_[start] == '\n' || s_[start] == '\t')) return false;
    std::size_t search = start;
    while (search < s_.size()) {
      const std::size_t found = s_.find(display ? "$$" : "$", search);
      if (found == std::string_view::npos) return false;
      if (found > start && s_[found - 1] == '\\') {
        search = found + 1;
        continue;
      }
      if (!display) {
        if (found == start || util::is_ascii_space(s_[found - 1]) || util::is_ascii_digit(peek(found + 1))) {
          search = found + 1;
          continue;
        }
      }
      if (found == start) return false;
      std::string content(s_.substr(start, found - start));
      append_child(root_, new_node(display ? NK::DisplayMath : NK::Math, std::move(content)));
      pos_ = found + open_len;
      return true;
    }
    return false;
  }

  bool parse_autolink() {
    // <scheme:...> or <email>
    const std::size_t close = s_.find('>', pos_ + 1);
    if (close == std::string_view::npos) return false;
    const std::string_view inner = std::string_view(s_).substr(pos_ + 1, close - pos_ - 1);
    if (inner.empty()) return false;
    // URI autolink.
    std::size_t colon = 0;
    while (colon < inner.size() && (util::is_ascii_alpha(inner[colon]) || (colon > 0 && (util::is_ascii_digit(inner[colon]) ||
                                                                                           inner[colon] == '+' || inner[colon] == '.' ||
                                                                                           inner[colon] == '-')))) {
      ++colon;
    }
    if (colon >= 2 && colon <= 32 && colon < inner.size() && inner[colon] == ':' &&
        std::ranges::none_of(inner, [](char c) { return static_cast<unsigned char>(c) <= 0x20 || c == '<'; })) {
      const int link = new_node(NK::Link, std::string(inner));
      const int label = new_node(NK::Text, std::string(inner));
      append_child(link, label);
      nodes_[static_cast<std::size_t>(link)].height = 1;
      append_child(root_, link);
      pos_ = close + 1;
      return true;
    }
    // Email autolink.
    const std::size_t at = inner.find('@');
    if (at != std::string_view::npos && at > 0 && at + 1 < inner.size()) {
      const std::string_view local = inner.substr(0, at);
      const std::string_view domain = inner.substr(at + 1);
      const auto local_ok = std::ranges::all_of(local, [](char c) {
        return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view(".!#$%&'*+/=?^_`{|}~-").find(c) != std::string_view::npos;
      });
      const auto domain_ok = std::ranges::all_of(domain, [](char c) {
        return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '.';
      });
      if (local_ok && domain_ok && !domain.starts_with(".") && !domain.ends_with(".") && !domain.starts_with("-")) {
        const int link = new_node(NK::Link, "mailto:" + std::string(inner));
        append_child(link, new_node(NK::Text, std::string(inner)));
        nodes_[static_cast<std::size_t>(link)].height = 1;
        append_child(root_, link);
        pos_ = close + 1;
        return true;
      }
    }
    return false;
  }

  bool parse_html() {
    const std::size_t length = match_raw_html(s_, pos_);
    if (length == 0) return false;
    append_child(root_, new_node(NK::RawHtml, std::string(s_.substr(pos_, length))));
    pos_ += length;
    return true;
  }

  // ---- delimiters ----
  void scan_flanking(char c, std::size_t start, std::size_t count, bool& can_open, bool& can_close) const {
    char32_t before = '\n';
    char32_t after = '\n';
    if (start > 0) {
      std::size_t b = start - 1;
      while (b > 0 && (static_cast<unsigned char>(s_[b]) & 0xC0) == 0x80) --b;
      before = util::decode_utf8(s_, b).code_point;
    }
    if (start + count < s_.size()) after = util::decode_utf8(s_, start + count).code_point;
    const bool after_space = is_unicode_whitespace(after);
    const bool after_punct = is_unicode_punctuation(after);
    const bool before_space = is_unicode_whitespace(before);
    const bool before_punct = is_unicode_punctuation(before);
    const bool left = !after_space && (!after_punct || before_space || before_punct);
    const bool right = !before_space && (!before_punct || after_space || after_punct);
    if (c == '_') {
      can_open = left && (!right || before_punct);
      can_close = right && (!left || after_punct);
    } else {
      can_open = left;
      can_close = right;
    }
  }

  bool parse_delimiters(char c) {
    std::size_t count = 0;
    while (pos_ + count < s_.size() && s_[pos_ + count] == c) ++count;
    bool can_open = false;
    bool can_close = false;
    scan_flanking(c, pos_, count, can_open, can_close);
    if (c == '~' && count > 2) {
      // Three or more tildes are never strikethrough.
      append_text(std::string_view(s_).substr(pos_, count));
      pos_ += count;
      return true;
    }
    const int node = append_special(std::string_view(s_).substr(pos_, count));
    pos_ += count;
    if (can_open || can_close) {
      Delimiter d;
      d.node = node;
      d.ch = c;
      d.count = static_cast<int>(count);
      d.original = static_cast<int>(count);
      d.can_open = can_open;
      d.can_close = can_close;
      d.prev = last_delimiter_;
      delimiters_.push_back(d);
      const int index = static_cast<int>(delimiters_.size() - 1);
      if (last_delimiter_ >= 0) delimiters_[static_cast<std::size_t>(last_delimiter_)].next = index;
      last_delimiter_ = index;
    }
    return true;
  }

  void remove_delimiter(int index) {
    Delimiter& d = delimiters_[static_cast<std::size_t>(index)];
    if (d.prev >= 0) delimiters_[static_cast<std::size_t>(d.prev)].next = d.next;
    if (d.next >= 0) {
      delimiters_[static_cast<std::size_t>(d.next)].prev = d.prev;
    } else {
      last_delimiter_ = d.prev;
    }
    d.prev = d.next = -1;
    d.node = -1;
  }

  void process_emphasis(int stack_bottom) {
    // openers_bottom indexed by [char][can_open][count % 3]
    std::map<std::tuple<char, bool, int>, int> openers_bottom;
    int closer = last_delimiter_;
    while (closer >= 0 && delimiters_[static_cast<std::size_t>(closer)].prev != stack_bottom) {
      closer = delimiters_[static_cast<std::size_t>(closer)].prev;
    }
    if (closer >= 0 && closer == stack_bottom) closer = -1;
    if (closer < 0 && last_delimiter_ >= 0) {
      // Find the first delimiter above the bottom.
      int d = last_delimiter_;
      int first = -1;
      while (d >= 0 && d != stack_bottom) {
        first = d;
        d = delimiters_[static_cast<std::size_t>(d)].prev;
      }
      closer = first;
    }
    while (closer >= 0) {
      Delimiter& c = delimiters_[static_cast<std::size_t>(closer)];
      if (!c.can_close) {
        closer = c.next;
        continue;
      }
      const auto key = std::make_tuple(c.ch, c.can_open, c.original % 3);
      const int bottom = openers_bottom.contains(key) ? openers_bottom[key] : stack_bottom;
      int opener = c.prev;
      bool found = false;
      while (opener >= 0 && opener != stack_bottom && opener != bottom) {
        const Delimiter& o = delimiters_[static_cast<std::size_t>(opener)];
        const bool odd = (c.can_open || o.can_close) && c.original % 3 != 0 && (o.original + c.original) % 3 == 0;
        if (o.ch == c.ch && o.can_open && !odd && (c.ch != '~' || o.count == c.count)) {
          found = true;
          break;
        }
        opener = o.prev;
      }
      const int old_closer = closer;
      if (c.ch == '*' || c.ch == '_' || c.ch == '~') {
        if (!found) {
          closer = c.next;
        } else {
          Delimiter& o = delimiters_[static_cast<std::size_t>(opener)];
          const int use = c.ch == '~' ? c.count : (c.count >= 2 && o.count >= 2 ? 2 : 1);
          const int opener_node = o.node;
          const int closer_node = c.node;
          const int inner_height = height_between(opener_node, closer_node);
          if (inner_height + 1 > max_height) {
            // Too deeply nested: leave the delimiters as text.
            closer = c.next;
            continue;
          }
          o.count -= use;
          c.count -= use;
          INode& on = nodes_[static_cast<std::size_t>(opener_node)];
          INode& cn = nodes_[static_cast<std::size_t>(closer_node)];
          on.text.erase(on.text.size() - static_cast<std::size_t>(use));
          cn.text.erase(cn.text.size() - static_cast<std::size_t>(use));
          const NK kind = c.ch == '~' ? NK::Strike : (use == 1 ? NK::Emph : NK::Strong);
          const int emph = new_node(kind);
          const int height = move_between(opener_node, closer_node, emph);
          nodes_[static_cast<std::size_t>(emph)].height = height + 1;
          insert_after(opener_node, emph);
          // Remove delimiters between opener and closer.
          int between = delimiters_[static_cast<std::size_t>(closer)].prev;
          while (between >= 0 && between != opener) {
            const int prev = delimiters_[static_cast<std::size_t>(between)].prev;
            remove_delimiter(between);
            between = prev;
          }
          if (delimiters_[static_cast<std::size_t>(opener)].count == 0) {
            unlink(opener_node);
            remove_delimiter(opener);
          }
          if (delimiters_[static_cast<std::size_t>(closer)].count == 0) {
            const int next = delimiters_[static_cast<std::size_t>(closer)].next;
            unlink(closer_node);
            remove_delimiter(closer);
            closer = next;
          }
        }
      } else {
        closer = c.next;
      }
      if (!found) {
        openers_bottom[key] = delimiters_[static_cast<std::size_t>(old_closer)].prev;
        if (!delimiters_[static_cast<std::size_t>(old_closer)].can_open) remove_delimiter(old_closer);
      }
    }
    // Remove all delimiters above the bottom.
    while (last_delimiter_ >= 0 && last_delimiter_ != stack_bottom) remove_delimiter(last_delimiter_);
  }

  // ---- links ----
  bool parse_open_bracket() {
    // Footnote reference "[^label]".
    if (state_.gfm() && peek(pos_ + 1) == '^') {
      const std::size_t close = s_.find(']', pos_ + 2);
      if (close != std::string_view::npos && close > pos_ + 2) {
        const std::string_view label = std::string_view(s_).substr(pos_ + 2, close - pos_ - 2);
        if (std::ranges::none_of(label, [](char x) { return util::is_ascii_space(x) || x == '['; })) {
          const std::string key = normalize_label(label);
          if (state_.footnote_labels.contains(key)) {
            state_.footnotes_used.insert(key);
            append_child(root_, new_node(NK::Note, key));
            pos_ = close + 1;
            return true;
          }
        }
      }
    }
    const int node = append_special("[");
    ++pos_;
    push_bracket(node, false);
    return true;
  }

  bool parse_bang() {
    if (peek(pos_ + 1) == '[') {
      const int node = append_special("![");
      pos_ += 2;
      push_bracket(node, true);
      return true;
    }
    append_text("!");
    ++pos_;
    return true;
  }

  void push_bracket(int node, bool image) {
    if (!brackets_.empty()) brackets_.back().bracket_after = true;
    Bracket b;
    b.node = node;
    b.image = image;
    b.previous_delimiter = last_delimiter_;
    b.index = pos_;
    brackets_.push_back(b);
  }

  bool parse_close_bracket() {
    ++pos_;
    if (brackets_.empty()) {
      append_text("]");
      return true;
    }
    Bracket opener = brackets_.back();
    if (!opener.active) {
      brackets_.pop_back();
      append_text("]");
      return true;
    }
    const std::size_t start_after = pos_;
    std::string destination;
    std::string title;
    bool matched = false;
    // Inline link "(destination "title")".
    if (peek(pos_) == '(') {
      std::size_t i = pos_ + 1;
      while (i < s_.size() && util::is_ascii_space(s_[i])) ++i;
      std::string dest;
      std::size_t after = i;
      if (i < s_.size() && s_[i] == ')') {
        after = i;
      } else {
        after = parse_destination(s_, i, dest);
      }
      if (after != std::string_view::npos) {
        std::size_t j = after;
        while (j < s_.size() && util::is_ascii_space(s_[j])) ++j;
        std::string t;
        if (j > after && j < s_.size() && (s_[j] == '"' || s_[j] == '\'' || s_[j] == '(')) {
          const std::size_t after_title = parse_title(s_, j, t);
          if (after_title != std::string_view::npos) {
            j = after_title;
            while (j < s_.size() && util::is_ascii_space(s_[j])) ++j;
          } else {
            t.clear();
          }
        }
        if (j < s_.size() && s_[j] == ')') {
          destination = std::move(dest);
          title = std::move(t);
          pos_ = j + 1;
          matched = true;
        }
      }
    }
    if (!matched) {
      // Reference link: [text][label], [text][], or [text].
      std::string label;
      const std::size_t after_label = parse_label(s_, pos_, label);
      std::string key;
      if (after_label != std::string_view::npos) {
        key = label.empty() ? std::string() : normalize_label(label);
        if (!label.empty()) pos_ = after_label;
      } else if (s_.substr(pos_).starts_with("[]")) {
        pos_ += 2;
      }
      if (key.empty() && !opener.bracket_after) {
        key = normalize_label(std::string_view(s_).substr(opener.index, start_after - 1 - opener.index));
      }
      if (!key.empty()) {
        if (const auto it = state_.references.find(key); it != state_.references.end()) {
          destination = it->second.url;
          title = it->second.title;
          matched = true;
        }
      }
      if (!matched) pos_ = start_after;
    }
    if (!matched) {
      brackets_.pop_back();
      append_text("]");
      return true;
    }
    const int inner_height = height_between(opener.node, -1);
    const int link = new_node(opener.image ? NK::Image : NK::Link, std::move(destination));
    nodes_[static_cast<std::size_t>(link)].title = std::move(title);
    // Emphasis inside the link text is processed first.
    process_emphasis(opener.previous_delimiter);
    const int height = move_between(opener.node, -1, link);
    nodes_[static_cast<std::size_t>(link)].height = std::max(height, inner_height) + 1;
    append_child(root_, link);
    unlink(opener.node);
    special_.erase(opener.node);
    brackets_.pop_back();
    // Links cannot contain links: deactivate earlier "[" openers.
    if (!opener.image) {
      for (Bracket& b : brackets_) {
        if (!b.image) b.active = false;
      }
    }
    if (nodes_[static_cast<std::size_t>(link)].height > max_height) {
      // Keep it but flatten when converting (see to_ir).
    }
    return true;
  }

  // ---- inline HTML pairs ----
  // <u>x</u>, <sup>, <sub>, <del>/<s>, <mark>, <ins>, <kbd> and <br> become nodes.
  void pair_html_tags(int parent) {
    std::vector<int> open;  // raw html open-tag nodes
    for (int current = nodes_[static_cast<std::size_t>(parent)].first; current >= 0;) {
      const int next = nodes_[static_cast<std::size_t>(current)].next;
      INode& n = nodes_[static_cast<std::size_t>(current)];
      if (n.kind != NK::RawHtml) {
        if (n.first >= 0 && n.height < max_height) pair_html_tags(current);
        current = next;
        continue;
      }
      const std::string tag = util::to_lower(n.text);
      if (tag == "<br>" || tag == "<br/>" || tag == "<br />") {
        n.kind = NK::HardBreak;
        n.text.clear();
      } else if (simple_tag(tag, false) != NK::Text) {
        open.push_back(current);
      } else if (const NK kind = simple_tag(tag, true); kind != NK::Text) {
        for (std::size_t k = open.size(); k-- > 0;) {
          const std::string open_tag = util::to_lower(nodes_[static_cast<std::size_t>(open[k])].text);
          if (simple_tag(open_tag, false) == kind) {
            const int opener = open[k];
            if (height_between(opener, current) + 1 > max_height) break;
            nodes_[static_cast<std::size_t>(opener)].kind = kind;
            nodes_[static_cast<std::size_t>(opener)].text.clear();
            const int height = move_between(opener, current, opener);
            nodes_[static_cast<std::size_t>(opener)].height = height + 1;
            unlink(current);
            open.resize(k);
            break;
          }
        }
      }
      current = next;
    }
  }

  static NK simple_tag(std::string_view tag, bool closing) {
    const std::string_view prefix = closing ? "</" : "<";
    if (!tag.starts_with(prefix) || !tag.ends_with(">")) return NK::Text;
    const std::string_view name = tag.substr(prefix.size(), tag.size() - prefix.size() - 1);
    if (name == "u" || name == "ins") return NK::Underline;
    if (name == "sup") return NK::Sup;
    if (name == "sub") return NK::Sub;
    if (name == "del" || name == "s" || name == "strike") return NK::Strike;
    if (name == "mark") return NK::Mark;
    if (name == "kbd") return NK::Code;
    return NK::Text;
  }

  // ---- conversion ----
  ir::Inlines to_ir(int parent, int depth) {
    ir::Inlines out;
    for (int current = nodes_[static_cast<std::size_t>(parent)].first; current >= 0;
         current = nodes_[static_cast<std::size_t>(current)].next) {
      INode& n = nodes_[static_cast<std::size_t>(current)];
      const auto children = [&] {
        if (depth > ir::max_document_depth - 4) {
          ir::Inlines flat;
          append_text_of(current, flat);
          return flat;
        }
        return to_ir(current, depth + 1);
      };
      switch (n.kind) {
        case NK::Text: emit_text(out, n.text, depth); break;
        case NK::SoftBreak: document::append_text(out, "\n"); break;
        case NK::HardBreak: out.push_back(ir::LineBreak{}); break;
        case NK::Code:
          if (n.first >= 0) {  // <kbd>...</kbd>
            ir::Inlines flat;
            append_text_of(current, flat);
            out.push_back(ir::Code{ir::plain_text(flat)});
          } else {
            out.push_back(ir::Code{n.text});
          }
          break;
        case NK::Math:
        case NK::DisplayMath: {
          ir::Code math{n.text};
          math.math = n.kind == NK::Math ? ir::MathMode::Inline : ir::MathMode::Display;
          out.push_back(std::move(math));
          break;
        }
        case NK::RawHtml: {
          ir::Code raw{n.text};
          raw.raw_format = "html";
          out.push_back(std::move(raw));
          break;
        }
        case NK::Emph: out.push_back(ir::Emphasis{children()}); break;
        case NK::Strong: out.push_back(ir::Strong{children()}); break;
        case NK::Strike: out.push_back(ir::Strikethrough{children()}); break;
        case NK::Underline: out.push_back(ir::Underline{children()}); break;
        case NK::Sup: out.push_back(ir::Superscript{children()}); break;
        case NK::Sub: out.push_back(ir::Subscript{children()}); break;
        case NK::Mark: {
          ir::Attributes attr;
          attr.add_class("mark");
          out.push_back(ir::Inline(ir::Strong{children()}, std::move(attr)));
          break;
        }
        case NK::Link: {
          const bool saved = in_link_;
          in_link_ = true;
          ir::Inlines label = children();
          in_link_ = saved;
          out.push_back(ir::Link{n.text, n.title, std::move(label)});
          break;
        }
        case NK::Image: {
          ir::Inlines alt_nodes;
          append_text_of(current, alt_nodes);
          ir::InlineImage image{image_source(n.text, state_), ir::plain_text(alt_nodes), n.title};
          out.push_back(std::move(image));
          break;
        }
        case NK::Note: {
          ir::Link link;
          link.note = state_.footnote_labels[n.text];
          link.url = "#fn-" + link.note;
          link.children = {ir::Text{link.note}};
          out.push_back(std::move(link));
          break;
        }
      }
    }
    return out;
  }

  void append_text_of(int parent, ir::Inlines& out) {
    std::vector<int> stack;
    for (int c = nodes_[static_cast<std::size_t>(parent)].last; c >= 0; c = nodes_[static_cast<std::size_t>(c)].prev) {
      stack.push_back(c);
    }
    while (!stack.empty()) {
      const int id = stack.back();
      stack.pop_back();
      const INode& n = nodes_[static_cast<std::size_t>(id)];
      if (n.kind == NK::Text || n.kind == NK::Code || n.kind == NK::Math) document::append_text(out, n.text);
      if (n.kind == NK::SoftBreak || n.kind == NK::HardBreak) document::append_text(out, " ");
      for (int c = n.last; c >= 0; c = nodes_[static_cast<std::size_t>(c)].prev) stack.push_back(c);
    }
  }

  // GFM extended autolinks in plain text: www., http(s)://, ftp:// and e-mail.
  void emit_text(ir::Inlines& out, const std::string& text, int depth) {
    (void)depth;
    if (!state_.gfm() || in_link_ || text.size() < 4) {
      document::append_text(out, text);
      return;
    }
    std::size_t i = 0;
    std::size_t literal = 0;
    while (i < text.size()) {
      const bool boundary = i == 0 || util::is_ascii_space(text[i - 1]) || text[i - 1] == '(' || text[i - 1] == '*' ||
                            text[i - 1] == '_' || text[i - 1] == '~' || text[i - 1] == '"' || text[i - 1] == '\'';
      std::size_t length = 0;
      std::string url;
      if (boundary) {
        const std::string_view rest = std::string_view(text).substr(i);
        if (rest.starts_with("www.")) {
          length = autolink_length(rest);
          url = "http://" + std::string(rest.substr(0, length));
        } else if (rest.starts_with("http://") || rest.starts_with("https://") || rest.starts_with("ftp://")) {
          const std::size_t scheme = rest.find("://") + 3;
          const std::size_t domain = autolink_length(rest.substr(scheme));
          if (domain > 0) {
            length = scheme + domain;
            url = std::string(rest.substr(0, length));
          }
        }
        if (length == 0) {
          const std::size_t email = email_length(rest);
          if (email > 0) {
            length = email;
            url = "mailto:" + std::string(rest.substr(0, email));
          }
        }
        if (length > 0 && url.size() > 7) {
          document::append_text(out, std::string_view(text).substr(literal, i - literal));
          const std::string label(std::string_view(text).substr(i, length));
          out.push_back(ir::Link{url, {}, {ir::Text{label}}});
          i += length;
          literal = i;
          continue;
        }
      }
      ++i;
    }
    document::append_text(out, std::string_view(text).substr(literal));
  }

  static std::size_t autolink_length(std::string_view s) {
    // Domain: segments of alnum, '-', '_' separated by '.', at least one period for www.
    std::size_t i = 0;
    std::size_t periods = 0;
    while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]) || s[i] == '-' || s[i] == '_' ||
                            s[i] == '.' || static_cast<unsigned char>(s[i]) >= 0x80)) {
      if (s[i] == '.') ++periods;
      ++i;
    }
    if (i == 0 || periods == 0) return 0;
    // Path: until whitespace or '<'.
    while (i < s.size() && !util::is_ascii_space(s[i]) && s[i] != '<') ++i;
    // Trailing punctuation is not part of the link.
    while (i > 0) {
      const char c = s[i - 1];
      if (c == '?' || c == '!' || c == '.' || c == ',' || c == ':' || c == '*' || c == '_' || c == '~' || c == '\'' ||
          c == '"') {
        --i;
        continue;
      }
      if (c == ')') {
        const auto open = std::ranges::count(s.substr(0, i), '(');
        const auto close = std::ranges::count(s.substr(0, i), ')');
        if (close > open) {
          --i;
          continue;
        }
      }
      if (c == ';') {
        // An entity-like "&name;" at the end is excluded.
        std::size_t k = i - 1;
        while (k > 0 && (util::is_ascii_alpha(s[k - 1]) || util::is_ascii_digit(s[k - 1]))) --k;
        if (k > 0 && s[k - 1] == '&') {
          i = k - 1;
          continue;
        }
      }
      break;
    }
    return i;
  }

  static std::size_t email_length(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]) || s[i] == '.' || s[i] == '-' ||
                            s[i] == '_' || s[i] == '+')) {
      ++i;
    }
    if (i == 0 || i >= s.size() || s[i] != '@') return 0;
    std::size_t j = i + 1;
    std::size_t periods = 0;
    while (j < s.size() && (util::is_ascii_alpha(s[j]) || util::is_ascii_digit(s[j]) || s[j] == '-' || s[j] == '_' ||
                            s[j] == '.')) {
      if (s[j] == '.') ++periods;
      ++j;
    }
    while (j > i + 1 && s[j - 1] == '.') {
      --j;
      --periods;
    }
    if (periods == 0 || s[j - 1] == '-' || s[j - 1] == '_') return 0;
    return j;
  }

  std::string_view s_;
  ParseState& state_;
  std::size_t pos_ = 0;
  std::vector<INode> nodes_;
  int root_ = -1;
  std::vector<Delimiter> delimiters_;
  int last_delimiter_ = -1;
  std::vector<Bracket> brackets_;
  std::set<int> special_;
  std::map<std::size_t, std::size_t> no_closer_;
  bool in_link_ = false;
};

}  // namespace

ir::Inlines parse_inlines(std::string_view text, ParseState& state) {
  Inliner inliner(text, state);
  return inliner.run();
}

}  // namespace cvt::document::markdown
