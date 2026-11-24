// Convertible core - document family: reStructuredText.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

constexpr int max_depth = 24;

bool adornment_char(char c) { return std::string_view("=-`:'\"~^_*+#<>.").find(c) != std::string_view::npos; }

bool is_adornment(std::string_view line) {
  const std::string_view t = util::trim_right(line);
  if (t.size() < 2 || !adornment_char(t.front())) return false;
  return std::ranges::all_of(t, [&](char c) { return c == t.front(); });
}

// ---- inline ------------------------------------------------------------------------------------

struct RstState {
  std::map<std::string, std::string> targets;        // normalised name -> url
  std::vector<std::string> anonymous;                // "__ url" targets in order
  std::size_t anonymous_used = 0;
  std::map<std::string, std::string> substitutions;  // |name| -> text
  std::vector<std::string> auto_labels;              // labels of "[#]" notes in order
  std::size_t auto_used = 0;
  std::size_t auto_used_defs = 0;
  std::map<std::string, bool> note_labels;           // labels with definitions
  ir::Document* document = nullptr;
};

std::string ref_name(std::string_view name) { return util::to_lower(collapse_whitespace(name)); }

bool end_ok(std::string_view text, std::size_t after) {
  if (after >= text.size()) return true;
  const char c = text[after];
  return util::is_ascii_space(c) || std::string_view("-.,:;!?\\/'\")]}>").find(c) != std::string_view::npos;
}

InlineSyntax rst_inline(RstState& state) {
  InlineSyntax syntax;
  syntax.rules = {
      {"**", "**", InlineKind::Strong},
      {"*", "*", InlineKind::Emphasis},
  };
  syntax.hook = [&state](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("\\")) {
      if (rest.size() >= 2 && (rest[1] == ' ' || rest[1] == '\n')) return 2;  // "\ " is nothing
      if (rest.size() >= 2) {
        append_text(out, rest.substr(1, 1));
        return 2;
      }
      return 0;
    }
    if (rest.starts_with("``")) {
      std::size_t search = 2;
      while (true) {
        const std::size_t end = rest.find("``", search);
        if (end == std::string_view::npos) return 0;
        if (end > 2 && !util::is_ascii_space(rest[end - 1]) && end_ok(rest, end + 2)) {
          std::string code(rest.substr(2, end - 2));
          for (char& c : code) {
            if (c == '\n') c = ' ';
          }
          out.push_back(ir::Code{std::move(code)});
          return end + 2;
        }
        search = end + 1;
      }
    }
    // :role:`text`
    if (rest.starts_with(":") && (pos == 0 || !util::is_ascii_alpha(text[pos - 1]))) {
      const std::size_t close = rest.find(":`", 1);
      if (close != std::string_view::npos && close < 30 &&
          std::ranges::all_of(rest.substr(1, close - 1), [](char c) { return util::is_ascii_alpha(c) || c == '-' || c == '_' || c == ':'; })) {
        const std::size_t end = rest.find('`', close + 2);
        if (end != std::string_view::npos && end > close + 2) {
          const std::string role(rest.substr(1, close - 1));
          const std::string_view content = rest.substr(close + 2, end - close - 2);
          ir::Inlines kids = parser.depth() < 20 ? parser.parse(content, parser.depth() + 1) : ir::Inlines{ir::Text{std::string(content)}};
          if (role == "sup" || role == "superscript") out.push_back(ir::Superscript{std::move(kids)});
          else if (role == "sub" || role == "subscript") out.push_back(ir::Subscript{std::move(kids)});
          else if (role == "math") {
            ir::Code math{std::string(content)};
            math.math = ir::MathMode::Inline;
            out.push_back(std::move(math));
          } else if (role == "code" || role == "literal" || role.starts_with("code-")) out.push_back(ir::Code{std::string(content)});
          else if (role == "strong") out.push_back(ir::Strong{std::move(kids)});
          else if (role == "del" || role == "strike" || role == "strikethrough") out.push_back(ir::Strikethrough{std::move(kids)});
          else if (role == "underline" || role == "ins") out.push_back(ir::Underline{std::move(kids)});
          else out.push_back(ir::Emphasis{std::move(kids)});
          return end + 1;
        }
      }
    }
    // `text <url>`_ / `name`_ / `text`
    if (rest.starts_with("`")) {
      const std::size_t end = rest.find('`', 1);
      if (end == std::string_view::npos || end == 1) return 0;
      const std::string_view inner = rest.substr(1, end - 1);
      std::size_t used = end + 1;
      int underscores = 0;
      while (underscores < 2 && used < rest.size() && rest[used] == '_') {
        ++used;
        ++underscores;
      }
      if (underscores == 0) {
        if (rest.substr(used).starts_with(":") && rest.find('`', used) == std::string_view::npos) {
          // a role after the text: `x`:sup:
          const std::size_t role_end = rest.find(':', used + 1);
          if (role_end != std::string_view::npos) {
            const std::string role(rest.substr(used + 1, role_end - used - 1));
            ir::Inlines kids{ir::Text{std::string(inner)}};
            if (role == "sup") out.push_back(ir::Superscript{std::move(kids)});
            else if (role == "sub") out.push_back(ir::Subscript{std::move(kids)});
            else out.push_back(ir::Emphasis{std::move(kids)});
            return role_end + 1;
          }
        }
        out.push_back(ir::Emphasis{{ir::Text{std::string(inner)}}});
        return used;
      }
      std::string label(inner);
      std::string url;
      if (label.ends_with(">")) {
        const std::size_t lt = label.rfind('<');
        if (lt != std::string::npos && (lt == 0 || util::is_ascii_space(label[lt - 1]))) {
          url = label.substr(lt + 1, label.size() - lt - 2);
          label = std::string(util::trim(std::string_view(label).substr(0, lt)));
          if (label.empty()) label = url;
          if (url.ends_with("_")) {
            const auto it = state.targets.find(ref_name(std::string_view(url).substr(0, url.size() - 1)));
            if (it != state.targets.end()) url = it->second;
          }
          if (underscores == 1) state.targets.emplace(ref_name(label), url);
        }
      }
      if (url.empty()) {
        if (underscores == 2 && state.anonymous_used < state.anonymous.size()) {
          url = state.anonymous[state.anonymous_used++];
        } else {
          const auto it = state.targets.find(ref_name(label));
          url = it != state.targets.end() ? it->second : "#" + slugify(label);
        }
      }
      std::string clean_url;
      for (const char c : url) {
        if (!util::is_ascii_space(c)) clean_url.push_back(c);
      }
      ir::Inlines kids = parser.depth() < 20 ? parser.parse(label, parser.depth() + 1) : ir::Inlines{ir::Text{label}};
      out.push_back(ir::Link{clean_url, {}, std::move(kids)});
      return used;
    }
    if (rest.starts_with("[STRIKEOUT:") && parser.depth() < 20) {
      const std::size_t close = rest.find(']');
      if (close != std::string_view::npos) {
        out.push_back(ir::Strikethrough{parser.parse(rest.substr(11, close - 11), parser.depth() + 1)});
        return close + 1;
      }
    }
    // Footnote references [1]_ [#]_ [#name]_ [*]_
    if (rest.starts_with("[")) {
      const std::size_t close = rest.find("]_");
      if (close != std::string_view::npos && close > 1 && close < 40 && rest.substr(1, close - 1).find(' ') == std::string_view::npos) {
        std::string label(rest.substr(1, close - 1));
        if (label == "#" || label == "*") {
          if (state.auto_used < state.auto_labels.size()) label = state.auto_labels[state.auto_used++];
        }
        if (state.note_labels.contains(label)) {
          ir::Link link;
          link.note = label;
          link.url = "#fn-" + label;
          link.children = {ir::Text{label}};
          out.push_back(std::move(link));
          return close + 2;
        }
      }
    }
    // |substitution|
    if (rest.starts_with("|") && rest.size() > 2 && !util::is_ascii_space(rest[1])) {
      const std::size_t end = rest.find('|', 1);
      if (end != std::string_view::npos) {
        const auto it = state.substitutions.find(std::string(rest.substr(1, end - 1)));
        if (it != state.substitutions.end()) {
          append_text(out, it->second);
          return end + 1 + (rest.substr(end + 1).starts_with("_") ? 1 : 0);
        }
      }
    }
    // name_ references
    if ((util::is_ascii_alpha(rest[0]) || util::is_ascii_digit(rest[0])) && (pos == 0 || !util::is_ascii_alpha(text[pos - 1]))) {
      std::size_t end = 0;
      while (end < rest.size() && (util::is_ascii_alpha(rest[end]) || util::is_ascii_digit(rest[end]) || rest[end] == '-')) ++end;
      if (end < rest.size() && rest[end] == '_' && end_ok(rest, end + 1) && !(end + 1 < rest.size() && rest[end + 1] == '_')) {
        const auto it = state.targets.find(ref_name(rest.substr(0, end)));
        if (it != state.targets.end()) {
          out.push_back(ir::Link{it->second, {}, {ir::Text{std::string(rest.substr(0, end))}}});
          return end + 1;
        }
      }
    }
    return wiki::common_inline(text, pos, out);
  };
  return syntax;
}

// ---- blocks -------------------------------------------------------------------------------------

class RstParser {
 public:
  RstParser(RstState& state) : state_(state), syntax_(rst_inline(state)) {}

  ir::Inlines inlines(std::string_view text) {
    InlineParser parser(syntax_);
    return parser.parse(util::trim(text));
  }

  // Lines of a block with its indentation removed.
  static std::vector<std::string> dedent(std::span<const std::string_view> lines) {
    std::size_t common = std::string::npos;
    for (const std::string_view l : lines) {
      if (!is_blank(l)) common = std::min(common, indentation(l));
    }
    std::vector<std::string> out;
    for (const std::string_view l : lines) out.emplace_back(strip_indent(l, common == std::string::npos ? 0 : common));
    while (!out.empty() && is_blank(out.back())) out.pop_back();
    return out;
  }

  // The indented block starting at `i` (lines indented more than `base` or blank).
  static std::size_t indented_end(std::span<const std::string_view> lines, std::size_t i, std::size_t base) {
    std::size_t end = i;
    std::size_t last_content = i;
    while (end < lines.size()) {
      if (is_blank(lines[end])) {
        ++end;
        continue;
      }
      if (indentation(lines[end]) <= base) break;
      last_content = ++end;
    }
    return last_content;
  }

  ir::Blocks parse(std::span<const std::string_view> lines, int depth) {
    ir::Blocks out;
    if (depth > max_depth) {
      out.push_back(ir::Paragraph{{ir::Text{join_lines(lines)}}});
      return out;
    }
    std::size_t i = 0;
    while (i < lines.size()) {
      if (is_blank(lines[i])) {
        ++i;
        continue;
      }
      const std::string_view line = lines[i];
      const std::size_t indent = indentation(line);
      if (indent > 0) {
        // Block quote.
        const std::size_t end = indented_end(lines, i, 0);
        std::vector<std::string> body = dedent(lines.subspan(i, end - i));
        std::vector<std::string_view> views(body.begin(), body.end());
        // Attribution "-- name" at the end is dropped.
        while (!views.empty() && (views.back().starts_with("-- ") || views.back().starts_with("\xE2\x80\x94 "))) views.pop_back();
        out.push_back(ir::BlockQuote{parse(views, depth + 1)});
        i = end;
        continue;
      }
      // Section titles.
      if (is_adornment(line) && i + 2 < lines.size() && !is_blank(lines[i + 1]) &&
          util::trim_right(lines[i + 2]) == util::trim_right(line)) {
        add_section(out, util::trim(lines[i + 1]), std::string(1, line.front()) + "o");
        i += 3;
        continue;
      }
      if (i + 1 < lines.size() && !is_adornment(line) && is_adornment(lines[i + 1]) &&
          util::trim_right(lines[i + 1]).size() >= std::min<std::size_t>(display_width(util::trim_right(line)), 3) &&
          !(i + 2 < lines.size() && util::trim_right(lines[i + 2]) == util::trim_right(lines[i + 1]))) {
        add_section(out, util::trim(line), std::string(1, util::trim(lines[i + 1]).front()));
        i += 2;
        continue;
      }
      // Transition.
      if (is_adornment(line) && util::trim(line).size() >= 4 && (i + 1 >= lines.size() || is_blank(lines[i + 1]))) {
        out.push_back(ir::HorizontalRule{});
        ++i;
        continue;
      }
      // Explicit markup.
      if (line.starts_with("..")) {
        i = explicit_markup(lines, i, out, depth);
        continue;
      }
      if (line.starts_with("__ ")) {
        ++i;
        continue;
      }
      // Tables.
      if (line.starts_with("+-") || line.starts_with("+=")) {
        i = grid_table(lines, i, out);
        continue;
      }
      if (line.starts_with("=") && util::trim(line).find(' ') != std::string_view::npos &&
          std::ranges::all_of(util::trim(line), [](char c) { return c == '=' || c == ' '; })) {
        i = simple_table(lines, i, out);
        continue;
      }
      // Lists.
      if (auto marker = list_marker(line)) {
        i = list(lines, i, out, depth, *marker);
        continue;
      }
      // Line blocks.
      if (line.starts_with("| ") || line == "|") {
        ir::Inlines content;
        while (i < lines.size() && (lines[i].starts_with("| ") || util::trim(lines[i]) == "|")) {
          if (!content.empty()) content.push_back(ir::LineBreak{});
          for (ir::Inline& x : inlines(lines[i].size() > 2 ? lines[i].substr(2) : std::string_view{})) content.push_back(std::move(x));
          ++i;
        }
        ir::Attributes attr;
        attr.add_class(ir::conventions::line_block);
        out.push_back(ir::Block(ir::Paragraph{std::move(content)}, std::move(attr)));
        continue;
      }
      // Field list.
      if (line.starts_with(":") && line.find(": ", 1) != std::string_view::npos && line.find('`') == std::string_view::npos) {
        i = field_list(lines, i, out, depth);
        continue;
      }
      // Definition list: a term line followed by an indented definition.
      if (i + 1 < lines.size() && !is_blank(lines[i + 1]) && indentation(lines[i + 1]) > 0) {
        i = definition_list(lines, i, out, depth);
        continue;
      }
      // Paragraph (possibly introducing a literal block with "::").
      std::vector<std::string_view> para;
      while (i < lines.size() && !is_blank(lines[i]) && indentation(lines[i]) == 0) para.push_back(lines[i++]);
      std::string text = join_lines(para);
      bool literal = false;
      if (util::trim_right(text).ends_with("::")) {
        literal = true;
        std::string_view t = util::trim_right(text);
        t.remove_suffix(2);
        if (t.empty() || util::is_ascii_space(t.back())) {
          text = std::string(util::trim_right(t));
        } else {
          text = std::string(t) + ":";
        }
      }
      if (!util::trim(text).empty()) {
        ir::Inlines content = inlines(text);
        if (!content.empty()) out.push_back(ir::Paragraph{std::move(content)});
      }
      if (literal) {
        while (i < lines.size() && is_blank(lines[i])) ++i;
        if (i < lines.size() && indentation(lines[i]) > 0) {
          const std::size_t end = indented_end(lines, i, 0);
          out.push_back(ir::CodeBlock{{}, util::join(dedent(lines.subspan(i, end - i)), "\n")});
          i = end;
        }
      }
    }
    return out;
  }

  ir::Document* document() { return state_.document; }

 private:
  void add_section(ir::Blocks& out, std::string_view title, const std::string& style) {
    const auto it = std::ranges::find(styles_, style);
    std::size_t level = static_cast<std::size_t>(it - styles_.begin());
    if (it == styles_.end()) {
      styles_.push_back(style);
      level = styles_.size() - 1;
    }
    // A unique overlined title at the very top is the document title.
    if (title_style_.empty() && out.empty() && state_.document != nullptr && state_.document->metadata.title.empty() &&
        style.size() == 2 && styles_.size() == 1 && !seen_section_) {
      title_style_ = style;
      state_.document->metadata.title = std::string(title);
      seen_section_ = true;
      return;
    }
    seen_section_ = true;
    const std::size_t offset = title_style_.empty() ? 0 : 1;
    const int heading_level = static_cast<int>(std::clamp<std::size_t>(level + 1 - std::min(level, offset), 1, 6));
    ir::Attributes attr;
    out.push_back(ir::Block(ir::Heading{level < offset ? 1 : heading_level, inlines(title)}, std::move(attr)));
  }

  struct Marker {
    std::size_t width = 0;  // marker plus following space
    bool ordered = false;
    int start = 1;
    ir::ListNumbering numbering = ir::ListNumbering::Decimal;
    char kind = 0;  // bullet char or enumerator style
  };

  static std::optional<Marker> list_marker(std::string_view line) {
    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && (line[1] == ' ' || line.size() == 1)) {
      return Marker{2, false, 1, ir::ListNumbering::Decimal, line[0]};
    }
    if (line.starts_with("\xE2\x80\xA2 ")) return Marker{4, false, 1, ir::ListNumbering::Decimal, 'o'};
    std::size_t i = 0;
    const bool paren = line.starts_with("(");
    if (paren) ++i;
    const std::size_t start = i;
    while (i < line.size() && util::is_ascii_digit(line[i])) ++i;
    Marker m;
    m.ordered = true;
    if (i > start && i - start < 10) {
      m.start = static_cast<int>(util::parse_int(line.substr(start, i - start)).value_or(1));
    } else if (i < line.size() && line[i] == '#') {
      ++i;
    } else if (i < line.size() && util::is_ascii_alpha(line[i]) && i + 1 < line.size() && !util::is_ascii_alpha(line[i + 1])) {
      const char c = line[i];
      m.numbering = c >= 'a' && c <= 'z' ? ir::ListNumbering::LowerAlpha : ir::ListNumbering::UpperAlpha;
      if (c == 'i' || c == 'I') m.numbering = c == 'i' ? ir::ListNumbering::LowerRoman : ir::ListNumbering::UpperRoman;
      m.start = c == 'i' || c == 'I' ? 1 : util::ascii_lower(c) - 'a' + 1;
      ++i;
    } else {
      return std::nullopt;
    }
    if (i >= line.size()) return std::nullopt;
    if (paren) {
      if (line[i] != ')') return std::nullopt;
    } else if (line[i] != '.' && line[i] != ')') {
      return std::nullopt;
    }
    m.kind = line[i];
    ++i;
    if (i >= line.size() || line[i] != ' ') return std::nullopt;
    m.width = i + 1;
    return m;
  }

  std::size_t list(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out, int depth, Marker first) {
    ir::List list;
    list.ordered = first.ordered;
    list.start = first.start;
    list.numbering = first.numbering;
    bool loose = false;
    while (i < lines.size()) {
      const auto marker = list_marker(lines[i]);
      if (!marker || marker->ordered != first.ordered || (!first.ordered && marker->kind != first.kind)) break;
      // Item: first line after the marker plus lines indented to the content column.
      std::vector<std::string> item_lines;
      item_lines.emplace_back(lines[i].substr(marker->width));
      std::size_t j = i + 1;
      std::size_t last = j;
      while (j < lines.size()) {
        if (is_blank(lines[j])) {
          item_lines.emplace_back();
          ++j;
          continue;
        }
        if (indentation(lines[j]) < 1) break;
        item_lines.emplace_back(strip_indent(lines[j], marker->width));
        last = ++j;
      }
      while (item_lines.size() > last - i) item_lines.pop_back();
      std::vector<std::string_view> views(item_lines.begin(), item_lines.end());
      list.items.push_back(parse(views, depth + 1));
      i = last;
      std::size_t k = i;
      while (k < lines.size() && is_blank(lines[k])) ++k;
      if (k > i && k < lines.size() && list_marker(lines[k])) loose = true;
      if (k < lines.size() && list_marker(lines[k]) && indentation(lines[k]) == 0) {
        i = k;
      } else {
        break;
      }
    }
    // Paragraph items separated by blank lines make a loose list only when
    // an item holds several paragraphs.
    list.loose = false;
    for (const ir::Blocks& item : list.items) {
      std::size_t paragraphs = 0;
      for (const ir::Block& b : item) paragraphs += b.is<ir::Paragraph>() ? 1 : 0;
      if (paragraphs > 1) list.loose = true;
    }
    (void)loose;
    out.push_back(std::move(list));
    return i;
  }

  std::size_t definition_list(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out, int depth) {
    ir::List list;
    list.definition = true;
    while (i < lines.size() && !is_blank(lines[i]) && indentation(lines[i]) == 0 && i + 1 < lines.size() &&
           !is_blank(lines[i + 1]) && indentation(lines[i + 1]) > 0) {
      std::string_view term = lines[i];
      // "term : classifier"
      if (const std::size_t colon = term.find(" : "); colon != std::string_view::npos) term = term.substr(0, colon);
      const std::size_t end = indented_end(lines, i + 1, 0);
      std::vector<std::string> body = dedent(lines.subspan(i + 1, end - i - 1));
      std::vector<std::string_view> views(body.begin(), body.end());
      ir::Blocks item;
      item.push_back(ir::Paragraph{inlines(term)});
      for (ir::Block& b : parse(views, depth + 1)) item.push_back(std::move(b));
      list.items.push_back(std::move(item));
      i = end;
      while (i < lines.size() && is_blank(lines[i])) ++i;
    }
    out.push_back(std::move(list));
    return i;
  }

  std::size_t field_list(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out, int depth) {
    ir::List list;
    list.definition = true;
    const bool docinfo = out.empty() && state_.document != nullptr;
    while (i < lines.size() && lines[i].starts_with(":")) {
      const std::size_t close = lines[i].find(": ", 1);
      const std::size_t close2 = lines[i].ends_with(":") ? lines[i].size() - 1 : std::string_view::npos;
      const std::size_t sep = close != std::string_view::npos ? close : close2;
      if (sep == std::string_view::npos) break;
      const std::string name(lines[i].substr(1, sep - 1));
      std::string value(sep + 1 < lines[i].size() ? util::trim(lines[i].substr(sep + 1)) : std::string_view{});
      const std::size_t end = indented_end(lines, i + 1, 0);
      for (std::size_t k = i + 1; k < end; ++k) value.append(" ").append(util::trim(lines[k]));
      const std::string key = util::to_lower(name);
      if (docinfo && (key == "author" || key == "authors" || key == "date" || key == "title" || key == "language" ||
                      key == "lang" || key == "keywords" || key == "description" || key == "subject")) {
        if (key == "author" || key == "authors") state_.document->metadata.author = value;
        else if (key == "date") state_.document->metadata.date = value;
        else if (key == "title") state_.document->metadata.title = value;
        else if (key == "language" || key == "lang") state_.document->metadata.language = value;
        else state_.document->metadata.extra[key] = value;
      } else {
        list.items.push_back({ir::Paragraph{inlines(name)}, ir::Paragraph{inlines(value)}});
      }
      i = end;
      (void)depth;
    }
    if (!list.items.empty()) out.push_back(std::move(list));
    return i;
  }

  std::size_t explicit_markup(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out, int depth) {
    const std::string_view line = lines[i];
    std::string_view head = util::trim(line.substr(2));
    // An empty comment ends the previous construct and consumes nothing else.
    if (head.empty() && (i + 1 >= lines.size() || is_blank(lines[i + 1]))) return i + 1;
    const std::size_t end = indented_end(lines, i + 1, 0);
    // Footnotes and citations.
    if (head.starts_with("[")) {
      const std::size_t close = head.find(']');
      if (close != std::string_view::npos) {
        std::string label(head.substr(1, close - 1));
        if (label == "#" || label == "*" || label.starts_with("#")) label = next_auto_label(label);
        std::vector<std::string> body;
        const std::string_view first = util::trim(head.substr(close + 1));
        if (!first.empty()) body.emplace_back(first);
        for (const std::string& l : dedent(lines.subspan(i + 1, end - i - 1))) body.push_back(l);
        std::vector<std::string_view> views(body.begin(), body.end());
        if (state_.document != nullptr) {
          state_.document->footnotes.push_back(ir::Footnote{label, parse(views, depth + 1)});
        }
        return end;
      }
    }
    if (head.starts_with("_") || head.starts_with("|")) return std::max(end, i + 1);  // targets/substitutions (pre-scanned)
    const std::size_t directive_end = head.find("::");
    if (directive_end == std::string_view::npos) {
      // A comment (and its indented body).
      return std::max(end, i + 1);
    }
    const std::string name = util::to_lower(util::trim(head.substr(0, directive_end)));
    const std::string argument(util::trim(head.substr(directive_end + 2)));
    // Options ":key: value" then content.
    std::vector<std::string> body = dedent(lines.subspan(i + 1, end - i - 1));
    std::map<std::string, std::string> options;
    std::size_t content_start = 0;
    while (content_start < body.size() && body[content_start].starts_with(":")) {
      const std::size_t close = body[content_start].find(':', 1);
      if (close == std::string::npos) break;
      options[util::to_lower(body[content_start].substr(1, close - 1))] = std::string(util::trim(std::string_view(body[content_start]).substr(close + 1)));
      ++content_start;
    }
    while (content_start < body.size() && is_blank(body[content_start])) ++content_start;
    std::vector<std::string_view> content(body.begin() + static_cast<std::ptrdiff_t>(content_start), body.end());
    if (name == "code" || name == "code-block" || name == "sourcecode" || name == "highlight") {
      out.push_back(ir::CodeBlock{argument, join_lines(content)});
    } else if (name == "math") {
      ir::CodeBlock math{{}, content.empty() ? argument : join_lines(content)};
      math.math = ir::MathMode::Display;
      out.push_back(std::move(math));
    } else if (name == "raw") {
      ir::CodeBlock raw{{}, join_lines(content)};
      raw.raw_format = util::to_lower(argument);
      out.push_back(std::move(raw));
    } else if (name == "image" || name == "figure") {
      ir::ImageBlock image{wiki::image_from_url(argument), options["alt"], {}, {}};
      if (name == "figure" && !content.empty()) image.caption = collapse_whitespace(join_lines(content));
      out.push_back(std::move(image));
    } else if (name == "note" || name == "warning" || name == "tip" || name == "important" || name == "caution" ||
               name == "danger" || name == "hint" || name == "attention" || name == "error" || name == "admonition" ||
               name == "seealso") {
      std::vector<std::string_view> all;
      std::vector<std::string> with_argument;
      if (!argument.empty() && name != "admonition") with_argument.push_back(argument);
      for (const std::string_view c : content) with_argument.emplace_back(c);
      for (const std::string& s : with_argument) all.push_back(s);
      ir::Attributes attr;
      attr.add_class(ir::conventions::admonition);
      attr.add_class(name);
      out.push_back(ir::Block(ir::BlockQuote{parse(all, depth + 1)}, std::move(attr)));
    } else if (name == "topic" || name == "sidebar" || name == "rubric" || name == "epigraph" || name == "pull-quote" ||
               name == "highlights" || name == "container" || name == "compound") {
      ir::Blocks inner = parse(content, depth + 1);
      if (!argument.empty() && name != "container") out.push_back(ir::Paragraph{{ir::Strong{inlines(argument)}}});
      if (name == "epigraph" || name == "pull-quote" || name == "highlights") {
        out.push_back(ir::BlockQuote{std::move(inner)});
      } else {
        for (ir::Block& b : inner) out.push_back(std::move(b));
      }
    } else if (name == "list-table") {
      list_table(content, options, out);
    }
    // contents, toctree, meta, include and other directives are dropped.
    return std::max(end, i + 1);
  }

  void list_table(std::span<const std::string_view> content, std::map<std::string, std::string>& options, ir::Blocks& out) {
    // "* - a\n  - b" rows.
    ir::TableBlock table;
    const std::size_t header_rows = static_cast<std::size_t>(std::clamp<std::int64_t>(util::parse_int(options["header-rows"]).value_or(0), 0, 100));
    for (const std::string_view l : content) {
      if (l.starts_with("* - ")) {
        table.rows.emplace_back();
        table.rows.back().cells.push_back(ir::TableCell{inlines(l.substr(4)), 1, 1});
      } else if (util::trim_left(l).starts_with("- ") && !table.rows.empty()) {
        table.rows.back().cells.push_back(ir::TableCell{inlines(util::trim_left(l).substr(2)), 1, 1});
      } else if (!is_blank(l) && !table.rows.empty() && !table.rows.back().cells.empty()) {
        for (ir::Inline& x : inlines(l)) {
          table.rows.back().cells.back().content.push_back(ir::Text{" "});
          table.rows.back().cells.back().content.push_back(std::move(x));
        }
      }
    }
    for (std::size_t r = 0; r < table.rows.size() && r < header_rows; ++r) table.rows[r].header = true;
    if (!table.rows.empty()) out.push_back(std::move(table));
  }

  std::string next_auto_label(const std::string& label) {
    if (label.size() > 1 && label.starts_with("#")) return label.substr(1);
    if (state_.auto_used_defs < state_.auto_labels.size()) return state_.auto_labels[state_.auto_used_defs++];
    return label;
  }

  std::size_t simple_table(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out) {
    // Column boundaries from the first border line.
    const std::string_view border = util::trim_right(lines[i]);
    std::vector<std::pair<std::size_t, std::size_t>> columns;
    for (std::size_t k = 0; k < border.size();) {
      if (border[k] == '=') {
        const std::size_t start = k;
        while (k < border.size() && border[k] == '=') ++k;
        columns.emplace_back(start, k);
      } else {
        ++k;
      }
    }
    ir::TableBlock table;
    std::size_t borders = 1;
    std::size_t j = i + 1;
    std::vector<std::vector<std::string>> rows;
    std::size_t header_end = 0;
    for (; j < lines.size(); ++j) {
      const std::string_view l = util::trim_right(lines[j]);
      if (!l.empty() && std::ranges::all_of(l, [](char c) { return c == '=' || c == ' '; })) {
        ++borders;
        if (borders == 2 && j + 1 < lines.size() && !is_blank(lines[j + 1])) {
          header_end = rows.size();
          continue;
        }
        ++j;
        break;
      }
      if (l.empty()) continue;
      std::vector<std::string> cells(columns.size());
      for (std::size_t c = 0; c < columns.size(); ++c) {
        const std::size_t from = columns[c].first;
        const std::size_t to = c + 1 < columns.size() ? columns[c + 1].first : l.size();
        if (from < l.size()) cells[c] = std::string(util::trim(l.substr(from, std::min(to, l.size()) - from)));
      }
      // A line with an empty first column continues the previous row.
      if (!rows.empty() && cells[0].empty()) {
        for (std::size_t c = 0; c < cells.size(); ++c) {
          if (!cells[c].empty()) rows.back()[c].append(" ").append(cells[c]);
        }
        continue;
      }
      rows.push_back(std::move(cells));
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
      ir::TableRow row;
      row.header = r < header_end;
      for (const std::string& cell : rows[r]) row.cells.push_back(ir::TableCell{inlines(cell), 1, 1});
      table.rows.push_back(std::move(row));
    }
    if (!table.rows.empty()) out.push_back(std::move(table));
    return j;
  }

  std::size_t grid_table(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out) {
    std::size_t j = i;
    std::vector<std::string_view> grid;
    while (j < lines.size() && (lines[j].starts_with("+") || lines[j].starts_with("|"))) grid.push_back(util::trim_right(lines[j++]));
    // Column boundaries: '+' positions of the first border.
    std::vector<std::size_t> pluses;
    for (std::size_t k = 0; k < grid.front().size(); ++k) {
      if (grid.front()[k] == '+') pluses.push_back(k);
    }
    if (pluses.size() < 2) {
      out.push_back(ir::CodeBlock{{}, join_lines(grid)});
      return j;
    }
    ir::TableBlock table;
    std::vector<std::string> current(pluses.size() - 1);
    bool has_content = false;
    std::size_t header_rows = 0;
    for (std::size_t r = 1; r < grid.size(); ++r) {
      const std::string_view l = grid[r];
      if (l.starts_with("+")) {
        if (has_content) {
          ir::TableRow row;
          for (std::string& cell : current) {
            row.cells.push_back(ir::TableCell{inlines(collapse_whitespace(cell)), 1, 1});
            cell.clear();
          }
          table.rows.push_back(std::move(row));
          has_content = false;
        }
        if (l.find('=') != std::string_view::npos && header_rows == 0) header_rows = table.rows.size();
        continue;
      }
      for (std::size_t c = 0; c + 1 < pluses.size(); ++c) {
        const std::size_t from = pluses[c] + 1;
        const std::size_t to = pluses[c + 1];
        if (from < l.size()) {
          current[c].append(" ").append(util::trim(l.substr(from, std::min(to, l.size()) - from)));
          has_content = true;
        }
      }
    }
    for (std::size_t r = 0; r < header_rows && r < table.rows.size(); ++r) table.rows[r].header = true;
    if (!table.rows.empty()) out.push_back(std::move(table));
    return j;
  }

  RstState& state_;
  InlineSyntax syntax_;
  std::vector<std::string> styles_;
  std::string title_style_;
  bool seen_section_ = false;
};

// Collects targets, substitutions and footnote labels before parsing.
void prescan(std::span<const std::string_view> lines, RstState& state) {
  int auto_number = 0;
  for (const std::string_view raw : lines) {
    const std::string_view line = util::trim_left(raw);
    if (line.starts_with(".. _") && line.find(':') != std::string_view::npos) {
      std::string_view rest = line.substr(4);
      std::size_t colon = rest.find(": ");
      if (rest.starts_with("`")) colon = rest.find("`: ") + 1;
      if (colon == std::string_view::npos || colon == 0) continue;
      std::string name(rest.substr(0, colon));
      if (name.starts_with("`") && name.ends_with("`")) name = name.substr(1, name.size() - 2);
      std::string url(util::trim(rest.substr(colon + 1)));
      state.targets.emplace(ref_name(name), url);
    } else if (line.starts_with("__ ")) {
      state.anonymous.emplace_back(util::trim(line.substr(3)));
    } else if (line.starts_with(".. __: ")) {
      state.anonymous.emplace_back(util::trim(line.substr(7)));
    } else if (line.starts_with(".. |")) {
      const std::size_t close = line.find('|', 4);
      if (close == std::string_view::npos) continue;
      const std::string name(line.substr(4, close - 4));
      const std::string_view rest = util::trim(line.substr(close + 1));
      if (rest.starts_with("replace::")) state.substitutions[name] = std::string(util::trim(rest.substr(9)));
      else if (rest.starts_with("unicode::")) state.substitutions[name] = std::string(util::trim(rest.substr(9)));
    } else if (line.starts_with(".. [")) {
      const std::size_t close = line.find(']');
      if (close == std::string_view::npos) continue;
      std::string label(line.substr(4, close - 4));
      if (label == "#" || label == "*") {
        label = "auto" + std::to_string(++auto_number);
        state.auto_labels.push_back(label);
      } else if (label.starts_with("#")) {
        label = label.substr(1);
      }
      state.note_labels[label] = true;
    }
  }
}

ir::Document read_rst(std::string_view text) {
  ir::Document document;
  RstState state;
  state.document = &document;
  const std::vector<std::string_view> lines = util::split_lines(text);
  prescan(lines, state);
  RstParser parser(state);
  document.blocks = parser.parse(lines, 0);
  if (!document.footnotes.empty()) number_footnotes(document);
  // The space that reStructuredText requires before a note reference is not text.
  for_each_inlines(document, [](ir::Inlines& inlines) {
    for (std::size_t k = 1; k < inlines.size(); ++k) {
      const auto* link = inlines[k].get_if<ir::Link>();
      if (link == nullptr || link->note.empty()) continue;
      if (auto* t = inlines[k - 1].get_if<ir::Text>(); t != nullptr && t->text.ends_with(" ")) t->text.pop_back();
    }
  });
  return document;
}

// ---- writing ----------------------------------------------------------------------------------

bool word_char(char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || static_cast<unsigned char>(c) >= 0x80; }

class RstWriter {
 public:
  RstWriter(const ir::Document& document, int wrap) : doc_(document), wrap_(wrap) {}

  std::string run() {
    std::string out;
    const ir::DocumentMetadata& meta = doc_.metadata;
    if (!meta.title.empty()) {
      const std::string title = escape(meta.title);
      const std::string rule(std::max<std::size_t>(display_width(title), 1), '=');
      out += rule + "\n" + title + "\n" + rule + "\n\n";
    }
    if (!meta.author.empty()) out += ":Author: " + escape(meta.author) + "\n";
    if (!meta.date.empty()) out += ":Date: " + escape(meta.date) + "\n";
    if (!meta.author.empty() || !meta.date.empty()) out += "\n";
    out += blocks(doc_.blocks, 0);
    for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
      std::string body = blocks(doc_.footnotes[i].blocks, 1);
      out += "\n\n.. [" + std::to_string(i + 1) + "]\n" + indent(body, "   ");
    }
    while (out.ends_with('\n')) out.pop_back();
    return out + "\n";
  }

 private:
  static std::string indent(const std::string& text, std::string_view prefix) {
    std::string out;
    bool first = true;
    for (const std::string_view line : util::split(text, '\n')) {
      if (!first) out.push_back('\n');
      if (!line.empty()) out.append(prefix).append(line);
      first = false;
    }
    return out;
  }

  std::string escape(std::string_view text) const {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      const char next = i + 1 < text.size() ? text[i + 1] : ' ';
      if (c == '\\' || c == '*' || c == '`' || c == '|' || (c == '_' && !word_char(next)) || (c == '[' && text.find("]_", i) != std::string_view::npos) ||
          (c == ':' && next != ' ' && i > 0 && text[i - 1] == ' ')) {
        out.push_back('\\');
      }
      out.push_back(c == '\n' ? ' ' : c);
    }
    return out;
  }

  // Lines that would start a construct get a leading backslash.
  std::string escape_line(const std::string& line) const {
    if (line.empty()) return line;
    const char c = line.front();
    if (line.starts_with("..") || line.starts_with("__ ") || line.starts_with("+-") ||
        ((c == '-' || c == '*' || c == '+') && line.size() > 1 && line[1] == ' ') || c == ':' || c == '|' ||
        is_adornment(line) || line.starts_with(">>>")) {
      return "\\" + line;
    }
    std::size_t digits = 0;
    while (digits < line.size() && util::is_ascii_digit(line[digits])) ++digits;
    if ((digits > 0 || line.starts_with("#")) && digits + 1 < line.size() + 1 &&
        (line[digits == 0 ? 1 : digits] == '.' || line[digits == 0 ? 1 : digits] == ')')) {
      return "\\" + line;
    }
    if (line.size() > 1 && util::is_ascii_alpha(line[0]) && (line[1] == '.' || line[1] == ')') && (line.size() == 2 || line[2] == ' ')) {
      return "\\" + line;
    }
    return line;
  }

  std::string paragraph(const ir::Inlines& content) {
    std::string text = inlines(content);
    if (wrap_ > 0) text = wrap_text(text, static_cast<std::size_t>(wrap_));
    std::string out;
    bool first = true;
    for (std::string_view line : util::split(text, '\n')) {
      while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      if (!first) out.push_back('\n');
      out.append(escape_line(std::string(line)));
      first = false;
    }
    // A paragraph ending in "::" would introduce a literal block.
    if (out.ends_with("::")) out.insert(out.size() - 1, "\\");
    for (char& c : out) {
      if (c == '\x01') c = ' ';
    }
    return out;
  }

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    for (const ir::Block& b : list) {
      std::string text = block(b, depth);
      if (text.empty()) continue;
      if (!out.empty()) out.append("\n\n");
      out.append(text);
    }
    return out;
  }

  std::string block(const ir::Block& block, int depth) {
    if (depth > ir::max_document_depth * 2) return {};
    std::string out;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            if (block.attr.has_class(ir::conventions::line_block)) {
              std::string text = inlines(node.content);
              for (const std::string_view line : util::split(text, '\n')) {
                if (!out.empty()) out.push_back('\n');
                out.append("| ").append(line);
              }
              return;
            }
            out = paragraph(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n' || c == '\x01') c = ' ';
            }
            text = escape_line(std::string(util::trim(text)));
            if (text.empty()) return;
            static constexpr std::array<char, 6> marks = {'=', '-', '~', '^', '"', '\''};
            out = text + "\n" + std::string(std::max<std::size_t>(display_width(text), 1), marks[static_cast<std::size_t>(std::clamp(node.level, 1, 6) - 1)]);
          } else if constexpr (std::same_as<T, ir::List>) {
            out = list(node, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (node.math != ir::MathMode::None) {
              out = ".. math::\n\n" + indent(node.code, "   ");
            } else if (!node.raw_format.empty()) {
              out = ".. raw:: " + node.raw_format + "\n\n" + indent(node.code, "   ");
            } else if (!node.language.empty()) {
              out = ".. code:: " + node.language + "\n\n" + indent(node.code, "   ");
            } else {
              out = "::\n\n" + indent(node.code, "   ");
            }
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            const std::string inner = blocks(node.blocks, depth + 1);
            if (block.attr.has_class(ir::conventions::admonition)) {
              std::string kind = "note";
              for (const std::string& c : block.attr.classes) {
                if (c != ir::conventions::admonition) kind = c;
              }
              std::erase_if(kind, [](char c) { return !util::is_ascii_alpha(c) && c != '-'; });
              out = ".. " + (kind.empty() ? std::string("note") : kind) + "::\n\n" + indent(inner, "   ");
            } else {
              out = "..\n\n" + indent(inner, "   ");
            }
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            const auto cells = table_strings(node, [this](const ir::Inlines& c) {
              std::string text = inlines(c);
              for (char& ch : text) {
                if (ch == '\x01') ch = ' ';
              }
              return text;
            });
            if (cells.empty()) return;
            const std::size_t header = header_row_count(node);
            out = render_grid_table(cells, header, {}, GridTableStyle{'=', true});
            while (out.ends_with('\n')) out.pop_back();
            if (!node.caption.empty()) out = ".. table:: " + inline_line(node.caption) + "\n\n" + indent(out, "   ");
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string url = image_url(node.source, true);
            if (url.empty()) {
              out = paragraph({ir::Text{node.caption.empty() ? node.alt : node.caption}});
              return;
            }
            out = (node.caption.empty() ? ".. image:: " : ".. figure:: ") + url;
            if (!node.alt.empty()) out += "\n   :alt: " + collapse_whitespace(node.alt);
            if (!node.caption.empty()) out += "\n\n   " + escape(node.caption);
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "----------";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out.clear();
          }
        },
        block.node);
    return out;
  }

  std::string list(const ir::List& list, int depth) {
    std::string out;
    if (list.definition) {
      for (const ir::Blocks& item : list.items) {
        if (item.empty()) continue;
        std::string term;
        if (const auto* p = item.front().get_if<ir::Paragraph>()) term = inlines(p->content);
        for (char& c : term) {
          if (c == '\n' || c == '\x01') c = ' ';
        }
        ir::Blocks rest(item.begin() + 1, item.end());
        std::string body = blocks(rest, depth + 1);
        if (body.empty()) body = "\\ ";
        if (!out.empty()) out.append("\n\n");
        out.append(escape_line(std::string(util::trim(term)))).append("\n").append(indent(body, "   "));
      }
      return out;
    }
    int number = list.start;
    const bool spaced = list.loose || std::ranges::any_of(list.items, [](const ir::Blocks& item) { return item.size() > 1; });
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      std::string marker = "- ";
      if (list.ordered) {
        switch (list.numbering) {
          case ir::ListNumbering::LowerAlpha: marker = std::string(1, static_cast<char>('a' + std::clamp(number - 1, 0, 25))) + ". "; break;
          case ir::ListNumbering::UpperAlpha: marker = std::string(1, static_cast<char>('A' + std::clamp(number - 1, 0, 25))) + ". "; break;
          default: marker = std::to_string(number) + ". "; break;
        }
        ++number;
      }
      if (list.task(i) == ir::TaskState::Done) marker += "\xE2\x98\x91 ";
      if (list.task(i) == ir::TaskState::Open) marker += "\xE2\x98\x90 ";
      const std::string pad(display_width(marker), ' ');
      std::string body = blocks(list.items[i], depth + 1);
      if (body.empty()) body = "\\ ";
      std::string text = marker;
      bool first = true;
      for (const std::string_view line : util::split(body, '\n')) {
        if (!first) {
          text.push_back('\n');
          if (!line.empty()) text.append(pad);
        }
        text.append(line);
        first = false;
      }
      if (!out.empty()) out.append(spaced ? "\n\n" : "\n");
      out.append(text);
    }
    return out;
  }

  static char first_char(const ir::Inline& item) {
    if (const auto* t = item.get_if<ir::Text>()) return t->text.empty() ? ' ' : t->text.front();
    return 'a';
  }

  // Inline markup needs non-word characters around it; "\ " joins it to words.
  std::string inlines(const ir::Inlines& list, int depth = 0) {
    std::string out;
    if (depth > ir::max_document_depth * 2) return out;
    for (std::size_t index = 0; index < list.size(); ++index) {
      const ir::Inline& item = list[index];
      const char following = index + 1 < list.size() ? first_char(list[index + 1]) : ' ';
      const auto wrap_markup = [&](const std::string& markup) {
        if (markup.empty()) return;
        if (!out.empty() && word_char(out.back())) out.append("\\ ");
        out.append(markup);
        if (word_char(following)) out.append("\\ ");
      };
      const auto delimited = [&](std::string_view open, std::string_view close, const ir::Inlines& kids) {
        std::string inner = inlines(kids, depth + 1);
        // Nested markup is not allowed in reStructuredText: keep the text.
        std::string flat = escape(ir::plain_text(kids));
        const std::string content = inner.find('`') != std::string::npos || inner.find("**") != std::string::npos ? flat : inner;
        const std::string_view trimmed = util::trim(content);
        if (trimmed.empty()) {
          out.append(content);
          return;
        }
        wrap_markup(std::string(open) + std::string(trimmed) + std::string(close));
      };
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            if constexpr (std::same_as<T, ir::Text>) {
              out.append(escape(node.text));
            } else if constexpr (std::same_as<T, ir::Emphasis>) {
              delimited("*", "*", node.children);
            } else if constexpr (std::same_as<T, ir::Strong>) {
              delimited("**", "**", node.children);
            } else if constexpr (std::same_as<T, ir::Strikethrough>) {
              delimited(":del:`", "`", node.children);
            } else if constexpr (std::same_as<T, ir::Underline>) {
              delimited(":underline:`", "`", node.children);
            } else if constexpr (std::same_as<T, ir::Superscript>) {
              delimited(":sup:`", "`", node.children);
            } else if constexpr (std::same_as<T, ir::Subscript>) {
              delimited(":sub:`", "`", node.children);
            } else if constexpr (std::same_as<T, ir::Code>) {
              std::string text = node.text;
              for (char& c : text) {
                if (c == '\n') c = ' ';
              }
              if (node.math != ir::MathMode::None) {
                wrap_markup(":math:`" + util::replace_all(text, "`", "\\`") + "`");
              } else if (!node.raw_format.empty()) {
                return;
              } else if (text.empty()) {
                return;
              } else if (text.find("``") == std::string::npos && !text.starts_with(" ") && !text.ends_with(" ") &&
                         !text.ends_with("`")) {
                std::string protected_text = text;
                for (char& c : protected_text) {
                  if (c == ' ') c = '\x01';
                }
                wrap_markup("``" + protected_text + "``");
              } else {
                wrap_markup(":code:`" + util::replace_all(util::replace_all(text, "\\", "\\\\"), "`", "\\`") + "`");
              }
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                const std::size_t n = footnote_index(doc_, node.note);
                if (n > 0) {
                  out.append(out.empty() || out.back() == ' ' ? "" : " ");
                  out.append("[" + std::to_string(n) + "]_");
                  return;
                }
              }
              const std::string label = escape(ir::plain_text(node.children));
              std::string url = node.url;
              std::erase_if(url, [](char c) { return util::is_ascii_space(c); });
              if (label == escape(node.url) && node.url.find("://") != std::string::npos) {
                wrap_markup(url);
                return;
              }
              std::string target = util::replace_all(util::replace_all(label, "<", "\\<"), "`", "\\`");
              if (target.empty()) target = url;
              for (char& c : target) {
                if (c == ' ') c = '\x01';
              }
              wrap_markup("`" + target + " <" + util::replace_all(url, ">", "%3E") + ">`__");
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              out.append("\n");
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              out.append(escape(node.alt));
            }
          },
          item.node);
    }
    return out;
  }

  const ir::Document& doc_;
  int wrap_;
};

int sniff_rst(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"rst", "rest"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  if (head.find(".. code::") != std::string_view::npos || head.find(".. code-block::") != std::string_view::npos) score += 3;
  if (head.find("\n.. ") != std::string_view::npos) score += 1;
  if (head.find(".. toctree::") != std::string_view::npos || head.find(".. note::") != std::string_view::npos) score += 3;
  if (head.find("`__") != std::string_view::npos || head.find(">`_") != std::string_view::npos) score += 2;
  if (head.find("\n====") != std::string_view::npos || head.find("\n----") != std::string_view::npos) score += 1;
  return score >= 5 ? 68 : 0;
}

}  // namespace

void register_rst(Registry& registry) {
  MarkupFormat rst;
  rst.id = "rst";
  rst.name = "reStructuredText";
  rst.description = "reStructuredText markup (Docutils, Sphinx).";
  rst.extensions = {"rst", "rest"};
  rst.mime = "text/x-rst";
  rst.popularity = 35;
  rst.sniff = sniff_rst;
  rst.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_rst(text); };
  rst.write = [](const ir::Document& document, WriteContext& ctx) -> Result<std::string> {
    RstWriter writer(document, static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000)));
    return writer.run();
  };
  rst.features = feature_wrap;
  register_markup(registry, std::move(rst));
}

}  // namespace cvt::document
