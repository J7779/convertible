// Convertible core - document family: Djot markup.
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

struct DjotState {
  std::map<std::string, std::string> references;  // label -> url
  std::map<std::string, ir::Blocks> notes;        // label -> body
  std::vector<std::string> note_order;
};

std::size_t find_matching(std::string_view text, std::size_t open, char open_char, char close_char) {
  int depth = 0;
  for (std::size_t i = open; i < text.size(); ++i) {
    if (text[i] == '\\') {
      ++i;
      continue;
    }
    if (text[i] == open_char) ++depth;
    if (text[i] == close_char && --depth == 0) return i;
  }
  return std::string_view::npos;
}

ir::Attributes parse_attributes(std::string_view inner) {
  ir::Attributes attr;
  // Split at spaces outside quotes.
  std::vector<std::string_view> parts;
  bool quoted = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= inner.size(); ++i) {
    if (i < inner.size() && inner[i] == '"' && (i == 0 || inner[i - 1] != '\\')) quoted = !quoted;
    if (i == inner.size() || (inner[i] == ' ' && !quoted)) {
      if (i > start) parts.push_back(inner.substr(start, i - start));
      start = i + 1;
    }
  }
  for (const std::string_view part : parts) {
    if (part.starts_with("#")) attr.id = std::string(part.substr(1));
    else if (part.starts_with(".")) attr.classes.emplace_back(part.substr(1));
    else if (const std::size_t eq = part.find('='); eq != std::string_view::npos) {
      std::string value(part.substr(eq + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      attr.pairs.emplace_back(std::string(part.substr(0, eq)), value);
    }
  }
  return attr;
}

InlineSyntax djot_inline(DjotState& state) {
  InlineSyntax syntax;
  syntax.escape = '\\';
  syntax.rules = {
      {"_", "_", InlineKind::Emphasis, false, true, true}, {"*", "*", InlineKind::Strong, false, true, true},
      {"^", "^", InlineKind::Superscript, false, false, true}, {"~", "~", InlineKind::Subscript, false, false, true},
  };
  syntax.hook = [&state](InlineParser& parser, std::string_view text, std::size_t pos, ir::Inlines& out) -> std::size_t {
    const std::string_view rest = text.substr(pos);
    if (rest.starts_with("\\\n") || rest == "\\") {
      out.push_back(ir::LineBreak{});
      return rest.size() >= 2 ? 2 : 1;
    }
    if (rest.starts_with("\\ ")) {
      append_text(out, "\xC2\xA0");
      return 2;
    }
    // Verbatim and math: `x`, ``x``, $`x`, $$`x`.
    std::size_t dollars = 0;
    while (dollars < 2 && dollars < rest.size() && rest[dollars] == '$') ++dollars;
    if (dollars < rest.size() && rest[dollars] == '`' && (dollars > 0 || rest.starts_with("`"))) {
      std::size_t ticks = 0;
      while (dollars + ticks < rest.size() && rest[dollars + ticks] == '`') ++ticks;
      const std::string fence(ticks, '`');
      std::size_t search = dollars + ticks;
      std::size_t end = std::string_view::npos;
      while (search < rest.size()) {
        const std::size_t found = rest.find(fence, search);
        if (found == std::string_view::npos) break;
        std::size_t run = 0;
        while (found + run < rest.size() && rest[found + run] == '`') ++run;
        if (run == ticks) {
          end = found;
          break;
        }
        search = found + run;
      }
      std::string content(rest.substr(dollars + ticks, (end == std::string_view::npos ? rest.size() : end) - dollars - ticks));
      if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' && (content[1] == '`' || content[content.size() - 2] == '`')) {
        content = content.substr(1, content.size() - 2);
      }
      for (char& c : content) {
        if (c == '\n') c = ' ';
      }
      ir::Code code{std::move(content)};
      if (dollars > 0) code.math = dollars == 2 ? ir::MathMode::Display : ir::MathMode::Inline;
      // Raw inline: `x`{=html}
      std::size_t used = end == std::string_view::npos ? rest.size() : end + ticks;
      if (rest.substr(used).starts_with("{=")) {
        const std::size_t close = rest.find('}', used);
        if (close != std::string_view::npos) {
          code.raw_format = std::string(rest.substr(used + 2, close - used - 2));
          used = close + 1;
        }
      }
      out.push_back(std::move(code));
      return used;
    }
    // Braced forms {_x_} {*x*} {-x-} {+x+} {=x=} {^x^} {~x~}.
    if (rest.size() >= 4 && rest[0] == '{' && std::string_view("_*-+=^~").find(rest[1]) != std::string_view::npos) {
      const char d = rest[1];
      const std::string close = std::string(1, d) + "}";
      const std::size_t end = rest.find(close, 2);
      if (end != std::string_view::npos && parser.depth() < 20) {
        ir::Inlines kids = parser.parse(rest.substr(2, end - 2), parser.depth() + 1);
        switch (d) {
          case '_': out.push_back(ir::Emphasis{std::move(kids)}); break;
          case '*': out.push_back(ir::Strong{std::move(kids)}); break;
          case '-': out.push_back(ir::Strikethrough{std::move(kids)}); break;
          case '+': out.push_back(ir::Underline{std::move(kids)}); break;
          case '^': out.push_back(ir::Superscript{std::move(kids)}); break;
          case '~': out.push_back(ir::Subscript{std::move(kids)}); break;
          default: {
            ir::Attributes attr;
            attr.add_class("mark");
            out.push_back(ir::Inline(ir::Strong{std::move(kids)}, std::move(attr)));
          }
        }
        return end + 2;
      }
    }
    // Inline attributes after an element are skipped.
    if (rest.starts_with("{") && !out.empty()) {
      const std::size_t end = rest.find('}');
      if (end != std::string_view::npos && end < 200 && rest.substr(1, end - 1).find('{') == std::string_view::npos &&
          (rest[1] == '#' || rest[1] == '.' || rest.substr(1, end - 1).find('=') != std::string_view::npos)) {
        return end + 1;
      }
    }
    if (rest.starts_with("<")) {
      const std::size_t end = rest.find('>');
      if (end != std::string_view::npos && end > 1) {
        const std::string_view inner = rest.substr(1, end - 1);
        if (inner.find(' ') == std::string_view::npos && (inner.find(':') != std::string_view::npos || inner.find('@') != std::string_view::npos)) {
          const std::string url = inner.find(':') == std::string_view::npos ? "mailto:" + std::string(inner) : std::string(inner);
          out.push_back(ir::Link{url, {}, {ir::Text{std::string(inner)}}});
          return end + 1;
        }
      }
    }
    const bool image = rest.starts_with("![");
    if (rest.starts_with("[") || image) {
      const std::size_t open = image ? 1 : 0;
      if (!image && rest.starts_with("[^")) {
        const std::size_t close = rest.find(']');
        if (close != std::string_view::npos) {
          const std::string label(rest.substr(2, close - 2));
          if (state.notes.contains(label)) {
            if (std::ranges::find(state.note_order, label) == state.note_order.end()) state.note_order.push_back(label);
            ir::Link link;
            link.note = label;
            link.url = "#fn-" + label;
            link.children = {ir::Text{label}};
            out.push_back(std::move(link));
            return close + 1;
          }
        }
      }
      const std::size_t close = find_matching(rest, open, '[', ']');
      if (close == std::string_view::npos || parser.depth() > 20) return 0;
      const std::string_view label = rest.substr(open + 1, close - open - 1);
      std::string url;
      std::size_t used = 0;
      if (close + 1 < rest.size() && rest[close + 1] == '(') {
        const std::size_t end = find_matching(rest, close + 1, '(', ')');
        if (end == std::string_view::npos) return 0;
        for (const char c : rest.substr(close + 2, end - close - 2)) {
          if (c != '\n') url.push_back(c);
        }
        used = end + 1;
      } else if (close + 1 < rest.size() && rest[close + 1] == '[') {
        const std::size_t end = rest.find(']', close + 2);
        if (end == std::string_view::npos) return 0;
        std::string ref(rest.substr(close + 2, end - close - 2));
        if (ref.empty()) ref = std::string(label);
        const auto it = state.references.find(ref);
        if (it == state.references.end()) return 0;
        url = it->second;
        used = end + 1;
      } else if (close + 1 < rest.size() && rest[close + 1] == '{' && !image) {
        // A span [text]{.mark} or with other attributes.
        const std::size_t end = rest.find('}', close + 1);
        if (end == std::string_view::npos) return 0;
        const ir::Attributes attr = parse_attributes(rest.substr(close + 2, end - close - 2));
        ir::Inlines kids = parser.parse(label, parser.depth() + 1);
        if (attr.has_class("mark")) {
          ir::Attributes mark;
          mark.add_class("mark");
          out.push_back(ir::Inline(ir::Strong{std::move(kids)}, std::move(mark)));
        } else if (attr.has_class("underline")) {
          out.push_back(ir::Underline{std::move(kids)});
        } else {
          for (ir::Inline& k : kids) out.push_back(std::move(k));
        }
        return end + 1;
      } else {
        return 0;
      }
      if (image) {
        out.push_back(ir::InlineImage{wiki::image_from_url(url), ir::plain_text(parser.parse(label, parser.depth() + 1))});
      } else {
        out.push_back(ir::Link{url, {}, parser.parse(label, parser.depth() + 1)});
      }
      // Attributes after a link: {title="..."}
      if (rest.substr(used).starts_with("{")) {
        const std::size_t end = rest.find('}', used);
        if (end != std::string_view::npos) {
          const ir::Attributes attr = parse_attributes(rest.substr(used + 1, end - used - 1));
          if (auto* link = out.back().get_if<ir::Link>()) link->title = std::string(attr.get("title"));
          used = end + 1;
        }
      }
      return used;
    }
    return 0;
  };
  return syntax;
}

class DjotParser {
 public:
  explicit DjotParser(DjotState& state) : state_(state), syntax_(djot_inline(state)) {}

  ir::Inlines inlines(std::string_view text) {
    InlineParser parser(syntax_);
    return parser.parse(util::trim(text));
  }

  struct Marker {
    std::size_t width = 0;
    bool ordered = false;
    bool definition = false;
    int start = 1;
    char kind = 0;
    ir::ListNumbering numbering = ir::ListNumbering::Decimal;
  };

  static std::optional<Marker> marker(std::string_view line) {
    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
      return Marker{2, false, false, 1, line[0]};
    }
    if (line.size() >= 2 && line[0] == ':' && line[1] == ' ') return Marker{2, false, true, 1, ':'};
    std::size_t i = line.starts_with("(") ? 1 : 0;
    const std::size_t start = i;
    while (i < line.size() && util::is_ascii_digit(line[i])) ++i;
    Marker m;
    m.ordered = true;
    if (i > start && i - start < 10) {
      m.start = static_cast<int>(util::parse_int(line.substr(start, i - start)).value_or(1));
    } else if (i < line.size() && util::is_ascii_alpha(line[i]) && i + 1 < line.size() && (line[i + 1] == '.' || line[i + 1] == ')')) {
      m.numbering = line[i] >= 'a' ? ir::ListNumbering::LowerAlpha : ir::ListNumbering::UpperAlpha;
      m.start = util::ascii_lower(line[i]) - 'a' + 1;
      ++i;
    } else {
      return std::nullopt;
    }
    if (i >= line.size() || (line[i] != '.' && line[i] != ')')) return std::nullopt;
    m.kind = line[i];
    ++i;
    if (i >= line.size() || line[i] != ' ') return std::nullopt;
    m.width = i + 1;
    return m;
  }

  ir::Blocks parse(std::span<const std::string_view> lines, int depth) {
    ir::Blocks out;
    if (depth > max_depth) {
      out.push_back(ir::Paragraph{{ir::Text{join_lines(lines)}}});
      return out;
    }
    ir::Attributes pending;
    std::size_t i = 0;
    const auto push = [&](ir::Block block) {
      if (!pending.empty()) block.attr = std::move(pending);
      pending = {};
      out.push_back(std::move(block));
    };
    while (i < lines.size()) {
      const std::string_view line = lines[i];
      const std::string_view t = util::trim(line);
      if (t.empty()) {
        ++i;
        continue;
      }
      if (t.starts_with("{") && t.ends_with("}") && t.find("} ") == std::string_view::npos) {
        ir::Attributes attr = parse_attributes(t.substr(1, t.size() - 2));
        if (!attr.id.empty()) pending.id = attr.id;
        for (std::string& c : attr.classes) pending.add_class(c);
        ++i;
        continue;
      }
      if (t.starts_with("#")) {
        std::size_t level = 0;
        while (level < t.size() && t[level] == '#') ++level;
        if (level <= 6 && level < t.size() && t[level] == ' ') {
          std::string text(t.substr(level + 1));
          ++i;
          while (i < lines.size() && !is_blank(lines[i])) text.append(" ").append(util::trim(lines[i++]));
          push(ir::Heading{static_cast<int>(level), inlines(text)});
          continue;
        }
      }
      if (t.starts_with("```")) {
        std::size_t ticks = 0;
        while (ticks < t.size() && t[ticks] == '`') ++ticks;
        const std::string fence(ticks, '`');
        const std::string info(util::trim(t.substr(ticks)));
        std::vector<std::string_view> code;
        ++i;
        while (i < lines.size() && !(util::trim(lines[i]).starts_with(fence) && util::trim(lines[i]).find_first_not_of('`') == std::string_view::npos)) {
          code.push_back(lines[i++]);
        }
        if (i < lines.size()) ++i;
        ir::CodeBlock block{{}, join_lines(code)};
        if (info.starts_with("=")) {
          block.raw_format = info.substr(1);
        } else {
          block.language = info;
          if (block.language.empty() && !pending.classes.empty()) {
            block.language = pending.classes.front();
            pending.classes.erase(pending.classes.begin());
          }
        }
        push(std::move(block));
        continue;
      }
      if (t.starts_with(":::")) {
        std::size_t colons = 0;
        while (colons < t.size() && t[colons] == ':') ++colons;
        const std::string kind(util::trim(t.substr(colons)));
        std::vector<std::string_view> inner;
        ++i;
        int nested = 0;
        while (i < lines.size()) {
          const std::string_view l = util::trim(lines[i]);
          if (l.starts_with(":::")) {
            std::size_t c = 0;
            while (c < l.size() && l[c] == ':') ++c;
            if (util::trim(l.substr(c)).empty() && c >= colons && nested == 0) break;
            if (!util::trim(l.substr(c)).empty()) ++nested;
            else if (nested > 0) --nested;
          }
          inner.push_back(lines[i++]);
        }
        if (i < lines.size()) ++i;
        ir::Blocks content = parse(inner, depth + 1);
        static constexpr std::array<std::string_view, 7> kinds = {"note", "warning", "tip", "important", "caution", "danger", "hint"};
        if (std::ranges::find(kinds, kind) != kinds.end()) {
          ir::Attributes attr;
          attr.add_class(ir::conventions::admonition);
          attr.add_class(kind);
          out.push_back(ir::Block(ir::BlockQuote{std::move(content)}, std::move(attr)));
        } else if ((kind == "caption" || (kind.empty() && pending.has_class("caption"))) && !out.empty()) {
          // pandoc figure captions
          if (auto* image = out.back().get_if<ir::ImageBlock>()) image->caption = ir::plain_text(content);
        } else {
          // Figures: a picture plus a caption div.
          if (content.size() == 2 && content[0].is<ir::ImageBlock>() && content[1].is<ir::ImageBlock>() == false) {
            for (ir::Block& b : content) out.push_back(std::move(b));
          } else {
            for (ir::Block& b : content) out.push_back(std::move(b));
          }
        }
        pending = {};
        continue;
      }
      if (t.starts_with(">")) {
        std::vector<std::string> inner;
        while (i < lines.size() && util::trim_left(lines[i]).starts_with(">")) {
          std::string_view q = util::trim_left(lines[i++]).substr(1);
          if (q.starts_with(" ")) q.remove_prefix(1);
          inner.emplace_back(q);
        }
        std::vector<std::string_view> views(inner.begin(), inner.end());
        push(ir::BlockQuote{parse(views, depth + 1)});
        continue;
      }
      if ((t.starts_with("*") || t.starts_with("-")) && t.size() >= 3 &&
          std::ranges::all_of(t, [&](char c) { return c == t.front() || c == ' '; }) &&
          std::ranges::count(t, t.front()) >= 3) {
        push(ir::HorizontalRule{});
        ++i;
        continue;
      }
      if (t.starts_with("|")) {
        i = table(lines, i, out);
        continue;
      }
      if (t.starts_with("[^") && t.find("]:") != std::string_view::npos) {
        const std::size_t close = t.find("]:");
        const std::string label(t.substr(2, close - 2));
        std::vector<std::string> body{std::string(util::trim(t.substr(close + 2)))};
        ++i;
        while (i < lines.size() && (is_blank(lines[i]) || indentation(lines[i]) > 0)) body.emplace_back(strip_indent(lines[i++], 2));
        while (!body.empty() && is_blank(body.back())) body.pop_back();
        std::vector<std::string_view> views(body.begin(), body.end());
        state_.notes[label] = parse(views, depth + 1);
        continue;
      }
      if (t.starts_with("[") && t.find("]:") != std::string_view::npos && !t.starts_with("[^")) {
        ++i;  // reference definitions were collected before
        continue;
      }
      if (t.starts_with("^ ") && !out.empty()) {
        if (auto* table = out.back().get_if<ir::TableBlock>()) {
          table->caption = inlines(t.substr(2));
          ++i;
          continue;
        }
      }
      if (indentation(line) == 0) {
        if (auto m = marker(line)) {
          i = list(lines, i, out, depth, *m);
          continue;
        }
      }
      // Paragraph.
      std::vector<std::string_view> para;
      while (i < lines.size() && !is_blank(lines[i])) para.push_back(util::trim(lines[i++]));
      ir::Inlines content = inlines(join_lines(para));
      if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
        auto* image = content.front().get_if<ir::InlineImage>();
        push(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, {}});
      } else if (!content.empty()) {
        push(ir::Paragraph{std::move(content)});
      }
    }
    return out;
  }

 private:
  std::size_t list(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out, int depth, Marker first) {
    ir::List list;
    list.ordered = first.ordered;
    list.definition = first.definition;
    list.start = first.start;
    list.numbering = first.numbering;
    bool any_task = false;
    std::vector<ir::TaskState> tasks;
    bool blank_between = false;
    while (i < lines.size()) {
      const auto m = marker(lines[i]);
      if (!m || m->ordered != first.ordered || m->definition != first.definition || m->kind != first.kind) break;
      std::vector<std::string> body;
      std::string first_line(lines[i].substr(m->width));
      ir::TaskState task = ir::TaskState::None;
      if (!first.ordered && !first.definition) {
        if (first_line.starts_with("[ ] ")) {
          task = ir::TaskState::Open;
          first_line.erase(0, 4);
        } else if (first_line.starts_with("[x] ") || first_line.starts_with("[X] ")) {
          task = ir::TaskState::Done;
          first_line.erase(0, 4);
        }
      }
      if (task != ir::TaskState::None) any_task = true;
      tasks.push_back(task);
      body.push_back(first_line);
      std::size_t j = i + 1;
      std::size_t last = j;
      while (j < lines.size()) {
        if (is_blank(lines[j])) {
          body.emplace_back();
          ++j;
          continue;
        }
        const bool continuation = indentation(lines[j]) > 0 || (!marker(lines[j]) && j == last && !is_blank(lines[j - 1]));
        if (!continuation) break;
        body.emplace_back(strip_indent(lines[j], m->width));
        last = ++j;
      }
      while (body.size() > last - i) body.pop_back();
      std::vector<std::string_view> views(body.begin(), body.end());
      ir::Blocks item;
      if (first.definition) {
        // ": term" then the definition after a blank line.
        std::size_t split = 0;
        while (split < views.size() && !is_blank(views[split])) ++split;
        item.push_back(ir::Paragraph{inlines(join_lines(std::span<const std::string_view>(views).first(split)))});
        for (ir::Block& b : parse(std::span<const std::string_view>(views).subspan(split), depth + 1)) item.push_back(std::move(b));
      } else {
        item = parse(views, depth + 1);
      }
      list.items.push_back(std::move(item));
      i = last;
      std::size_t k = i;
      while (k < lines.size() && is_blank(lines[k])) ++k;
      const auto next = k < lines.size() ? marker(lines[k]) : std::nullopt;
      const bool same_list = next && next->ordered == first.ordered && next->definition == first.definition && next->kind == first.kind;
      if (k > i && same_list) blank_between = true;
      if (k < lines.size() && indentation(lines[k]) == 0 && marker(lines[k])) i = k;
      else break;
    }
    if (any_task) list.tasks = std::move(tasks);
    list.loose = blank_between && !first.definition;
    out.push_back(std::move(list));
    return i;
  }

  std::size_t table(std::span<const std::string_view> lines, std::size_t i, ir::Blocks& out) {
    ir::TableBlock table;
    while (i < lines.size() && util::trim(lines[i]).starts_with("|")) {
      std::string_view row = util::trim(lines[i]);
      row.remove_prefix(1);
      if (row.ends_with("|")) row.remove_suffix(1);
      std::vector<std::string> cells;
      std::string cell;
      for (std::size_t k = 0; k < row.size(); ++k) {
        if (row[k] == '\\' && k + 1 < row.size()) {
          cell.push_back(row[k]);
          cell.push_back(row[++k]);
          continue;
        }
        if (row[k] == '|') {
          cells.push_back(std::string(util::trim(cell)));
          cell.clear();
          continue;
        }
        cell.push_back(row[k]);
      }
      cells.push_back(std::string(util::trim(cell)));
      const bool separator = std::ranges::all_of(cells, [](const std::string& c) {
        return !c.empty() && std::ranges::all_of(c, [](char x) { return x == '-' || x == ':'; });
      });
      if (separator) {
        for (ir::TableRow& r : table.rows) r.header = true;
        table.alignments.clear();
        for (const std::string& c : cells) {
          const bool left = c.front() == ':';
          const bool right = c.back() == ':';
          table.alignments.push_back(left && right ? ir::Alignment::Center
                                     : right       ? ir::Alignment::Right
                                     : left        ? ir::Alignment::Left
                                                   : ir::Alignment::Default);
        }
      } else {
        ir::TableRow r;
        for (const std::string& c : cells) r.cells.push_back(ir::TableCell{inlines(c), 1, 1});
        table.rows.push_back(std::move(r));
      }
      ++i;
    }
    if (!table.rows.empty()) out.push_back(std::move(table));
    return i;
  }

  DjotState& state_;
  InlineSyntax syntax_;
};

ir::Document read_djot(std::string_view text) {
  DjotState state;
  const std::vector<std::string_view> lines = util::split_lines(text);
  for (const std::string_view raw : lines) {
    const std::string_view t = util::trim(raw);
    if (t.starts_with("[") && !t.starts_with("[^") && t.find("]:") != std::string_view::npos) {
      const std::size_t close = t.find("]:");
      state.references.emplace(std::string(t.substr(1, close - 1)), std::string(util::trim(t.substr(close + 2))));
    }
    if (t.starts_with("[^") && t.find("]:") != std::string_view::npos) {
      state.notes.emplace(std::string(t.substr(2, t.find("]:") - 2)), ir::Blocks{});
    }
  }
  DjotParser parser(state);
  ir::Document document;
  document.blocks = parser.parse(lines, 0);
  for (const std::string& label : state.note_order) {
    document.footnotes.push_back(ir::Footnote{label, std::move(state.notes[label])});
  }
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

// ---- writing -----------------------------------------------------------------------------------

std::string djot_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (std::string_view("\\*_[]{}<>`~^$!|#:+=-\"'").find(c) != std::string_view::npos ||
        (c == '.' && text.substr(i).starts_with("..."))) {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

class DjotWriter {
 public:
  DjotWriter(const ir::Document& document, int wrap) : doc_(document), wrap_(wrap) {
    m_.escape = djot_escape;
    m_.emphasis = {"{_", "_}"};
    m_.strong = {"{*", "*}"};
    m_.strikethrough = {"{-", "-}"};
    m_.underline = {"{+", "+}"};
    m_.mark = {"{=", "=}"};
    m_.superscript = {"{^", "^}"};
    m_.subscript = {"{~", "~}"};
    m_.line_break = "\\\n";
    m_.code = [](const ir::Code& code) -> std::string {
      std::string text = code.text;
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      std::size_t longest = 0;
      std::size_t run = 0;
      for (const char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
      }
      const std::string ticks(longest + 1, '`');
      const bool pad = text.starts_with("`") || text.ends_with("`");
      std::string out = ticks + (pad ? " " : "") + text + (pad ? " " : "") + ticks;
      for (char& c : out) {
        if (c == ' ') c = '\x01';
      }
      if (code.math == ir::MathMode::Inline) return "$" + out;
      if (code.math == ir::MathMode::Display) return "$$" + out;
      if (!code.raw_format.empty()) return out + "{=" + code.raw_format + "}";
      return out;
    };
    m_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      std::string url;
      for (const char c : link.url) {
        if (c == ')' || c == '(' || c == '\\') url.push_back('\\');
        if (!util::is_ascii_space(c)) url.push_back(c);
      }
      std::string out = "[" + label + "](" + url + ")";
      if (!link.title.empty()) out += "{title=\"" + util::replace_all(link.title, "\"", "\\\"") + "\"}";
      return out;
    };
    m_.image = [](const ir::InlineImage& image) -> std::string {
      const std::string url = image_url(image.source, true);
      if (url.empty()) return djot_escape(image.alt);
      return "![" + djot_escape(image.alt) + "](" + url + ")";
    };
    m_.note = [](const ir::Link&, std::size_t index) { return "[^" + std::to_string(index) + "]"; };
  }

  std::string run() {
    std::string out = blocks(doc_.blocks, 0);
    for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
      out += "\n\n[^" + std::to_string(i + 1) + "]: " + indent(blocks(doc_.footnotes[i].blocks, 1), "  ", false);
    }
    while (out.ends_with('\n')) out.pop_back();
    return out + "\n";
  }

 private:
  static std::string indent(const std::string& text, std::string_view prefix, bool first_too) {
    std::string out;
    bool first = true;
    for (const std::string_view line : util::split(text, '\n')) {
      if (!first) out.push_back('\n');
      if (!line.empty() && (!first || first_too)) out.append(prefix);
      out.append(line);
      first = false;
    }
    return out;
  }

  std::string inlines(const ir::Inlines& content) { return render_inlines(content, m_, &doc_); }

  std::string paragraph(const ir::Inlines& content) {
    std::string text = inlines(content);
    if (wrap_ > 0) text = wrap_text(text, static_cast<std::size_t>(wrap_));
    std::string out;
    bool first = true;
    for (std::string_view line : util::split(text, '\n')) {
      while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      if (!first) out.push_back('\n');
      std::string l(line);
      if (!l.empty() && util::is_ascii_digit(l.front())) {
        std::size_t d = 0;
        while (d < l.size() && util::is_ascii_digit(l[d])) ++d;
        if (d < l.size() && (l[d] == '.' || l[d] == ')')) l.insert(d, "\\");
      }
      out.append(l);
      first = false;
    }
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
            out = paragraph(node.content);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n' || c == '\x01') c = ' ';
            }
            if (!block.attr.id.empty() && block.attr.id != slugify(inline_line(node.content))) out = "{#" + block.attr.id + "}\n";
            out += std::string(static_cast<std::size_t>(std::clamp(node.level, 1, 6)), '#') + " " + std::string(util::trim(text));
          } else if constexpr (std::same_as<T, ir::List>) {
            out = list(node, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            std::size_t longest = 2;
            std::size_t run = 0;
            for (const char c : node.code) {
              run = c == '`' ? run + 1 : 0;
              longest = std::max(longest, run);
            }
            const std::string fence(longest + 1, '`');
            std::string info = node.raw_format.empty() ? node.language : "=" + node.raw_format;
            if (node.math != ir::MathMode::None) {
              out = "$$`" + util::replace_all(node.code, "\n", " ") + "`";
              return;
            }
            std::erase_if(info, [](char c) { return util::is_ascii_space(c) || c == '`'; });
            out = fence + info + "\n" + node.code + (node.code.empty() ? "" : "\n") + fence;
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            std::string inner = blocks(node.blocks, depth + 1);
            if (block.attr.has_class(ir::conventions::admonition)) {
              std::string kind = "note";
              for (const std::string& c : block.attr.classes) {
                if (c != ir::conventions::admonition) kind = c;
              }
              out = "::: " + kind + "\n" + inner + "\n:::";
              return;
            }
            for (const std::string_view line : util::split(inner, '\n')) {
              if (!out.empty()) out.push_back('\n');
              out.append(line.empty() ? ">" : "> ").append(line);
            }
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            const auto cells = table_strings(node, [this](const ir::Inlines& c) {
              std::string text = inlines(c);
              for (char& ch : text) {
                if (ch == '\n' || ch == '\x01') ch = ' ';
              }
              return text;
            });
            const std::size_t header = header_row_count(node);
            std::size_t columns = 0;
            for (const auto& row : cells) columns = std::max(columns, row.size());
            for (std::size_t r = 0; r < cells.size(); ++r) {
              if (!out.empty()) out.push_back('\n');
              out.push_back('|');
              for (std::size_t c = 0; c < columns; ++c) out.append(" ").append(c < cells[r].size() ? cells[r][c] : "").append(" |");
              if (header > 0 && r + 1 == header) {
                out.append("\n|");
                for (std::size_t c = 0; c < columns; ++c) {
                  const ir::Alignment a = node.column_alignment(c);
                  out.append(a == ir::Alignment::Center ? ":-:|" : a == ir::Alignment::Right ? "--:|" : a == ir::Alignment::Left ? ":--|" : "---|");
                }
              }
            }
            if (!node.caption.empty()) out += "\n^ " + inlines(node.caption);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string url = image_url(node.source, true);
            if (url.empty()) {
              out = djot_escape(node.caption.empty() ? node.alt : node.caption);
              return;
            }
            out = "![" + djot_escape(node.caption.empty() ? node.alt : node.caption) + "](" + url + ")";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "* * *";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out.clear();
          }
        },
        block.node);
    return out;
  }

  std::string list(const ir::List& list, int depth) {
    std::string out;
    int number = list.start;
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      const ir::Blocks& item = list.items[i];
      std::string marker;
      std::string body;
      if (list.definition) {
        marker = ": ";
        std::string term;
        if (!item.empty()) {
          if (const auto* p = item.front().get_if<ir::Paragraph>()) term = paragraph(p->content);
        }
        ir::Blocks rest(item.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(item.size())), item.end());
        body = term + "\n\n" + blocks(rest, depth + 1);
      } else {
        marker = list.ordered ? std::to_string(number++) + ". " : "- ";
        if (list.task(i) == ir::TaskState::Done) marker += "[x] ";
        if (list.task(i) == ir::TaskState::Open) marker += "[ ] ";
        body = blocks(item, depth + 1);
      }
      if (!out.empty()) out.append(list.loose || list.definition ? "\n\n" : "\n");
      // Nested blocks need a blank line before them inside tight items.
      if (!list.loose && !list.definition) body = util::replace_all(body, "\n\n", "\n\n");
      out.append(marker).append(indent(body, std::string(list.ordered ? std::to_string(number - 1).size() + 2 : 2, ' '), false));
    }
    return out;
  }

  const ir::Document& doc_;
  int wrap_;
  InlineMarkup m_;
};

int sniff_djot(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"dj", "djot"})) return 80;
  (void)data;
  return 0;
}

}  // namespace

void register_djot(Registry& registry) {
  MarkupFormat djot;
  djot.id = "djot";
  djot.name = "Djot";
  djot.description = "Djot light markup (a successor of CommonMark by its author).";
  djot.extensions = {"dj", "djot"};
  djot.mime = "text/x-djot";
  djot.popularity = 8;
  djot.sniff = sniff_djot;
  djot.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_djot(text); };
  djot.write = [](const ir::Document& document, WriteContext& ctx) -> Result<std::string> {
    DjotWriter writer(document, static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000)));
    return writer.run();
  };
  djot.features = feature_wrap;
  register_markup(registry, std::move(djot));
}

}  // namespace cvt::document
