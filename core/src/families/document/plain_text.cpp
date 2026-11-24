// Convertible core - document family: documents as well laid-out plain text,
// and structure inferred from plain text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <span>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

class TextRenderer {
 public:
  TextRenderer(const ir::Document& document, const PlainTextOptions& options) : doc_(document), options_(options) {}

  std::string run() {
    std::string out = blocks(doc_.blocks, 0, 0);
    std::string notes;
    for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
      const std::string marker = "[" + std::to_string(note_number(i + 1)) + "] ";
      std::string body = blocks(doc_.footnotes[i].blocks, marker.size(), 0);
      body = std::string(util::trim_left(body));
      if (!notes.empty()) notes.append("\n");
      notes.append(marker).append(body);
      if (!notes.ends_with("\n")) notes.append("\n");
    }
    std::string links;
    for (const auto& [number, url] : link_targets_) {
      links.append("[").append(std::to_string(number)).append("] ").append(url).append("\n");
    }
    const std::string rule(static_cast<std::size_t>(std::clamp(options_.wrap_width > 0 ? options_.wrap_width : 40, 10, 72)), '-');
    if (!notes.empty()) {
      if (!out.empty()) out.append("\n").append(rule).append("\n\n");
      out.append(notes);
    }
    if (!links.empty()) {
      if (!out.empty()) out.append("\n").append(notes.empty() ? rule + "\n\n" : "");
      out.append(links);
    }
    while (out.ends_with("\n\n")) out.pop_back();
    if (!out.empty() && !out.ends_with("\n")) out.push_back('\n');
    if (options_.newline != "\n") {
      out = util::convert_line_endings(out, options_.newline == "\r\n" ? util::LineEnding::CrLf : util::LineEnding::Cr);
    }
    return out;
  }

 private:
  std::size_t width_for(std::size_t indent) const {
    if (options_.wrap_width <= 0) return 0;
    const auto width = static_cast<std::size_t>(options_.wrap_width);
    return indent + 20 < width ? width : indent + 20;
  }

  // Notes and links share one numbering in order of appearance.
  int note_number(std::size_t index) {
    const auto it = note_numbers_.find(index);
    if (it != note_numbers_.end()) return it->second;
    const int number = ++counter_;
    note_numbers_[index] = number;
    return number;
  }

  std::string blocks(const ir::Blocks& list, std::size_t indent, int depth) {
    std::string out;
    for (const ir::Block& block : list) {
      std::string text = this->block(block, indent, depth);
      if (text.empty()) continue;
      if (!out.empty()) out.append("\n");
      out.append(text);
    }
    return out;
  }

  // Every block ends with '\n'; blocks are separated by one blank line.
  std::string block(const ir::Block& block, std::size_t indent, int depth) {
    if (depth > ir::max_document_depth * 2) return {};
    const std::string pad(indent, ' ');
    std::string out;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            out = wrap_text(inlines(node.content), width_for(indent), pad, pad) + "\n";
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            out = pad + text + "\n";
            if (options_.underline_headings && node.level <= 2 && !text.empty()) {
              out += pad + std::string(display_width(text), node.level == 1 ? '=' : '-') + "\n";
            }
          } else if constexpr (std::same_as<T, ir::List>) {
            out = list(node, indent, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty()) {
              // Raw markup: HTML is shown as its text, other formats are left out.
              if (node.raw_format == "html" && depth < 8) {
                if (auto parsed = parse_html_text(node.code); parsed && !parsed->blocks.empty()) {
                  out = blocks(parsed->blocks, indent, depth + 1);
                }
              }
              return;
            }
            for (const std::string_view line : util::split(node.code, '\n')) {
              out.append(pad).append("    ").append(line);
              while (out.ends_with(" ")) out.pop_back();
              out.append("\n");
            }
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            std::string inner = blocks(node.blocks, 0, depth + 1);
            while (inner.ends_with('\n')) inner.pop_back();
            for (const std::string_view line : util::split(inner, '\n')) {
              out.append(pad).append(line.empty() ? ">" : "> ").append(line).append("\n");
            }
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = table(node, pad);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string& text = node.caption.empty() ? node.alt : node.caption;
            out = pad + "[" + (text.empty() ? std::string("Image") : "Image: " + text) + "]\n";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            const auto width = static_cast<std::size_t>(std::clamp(options_.wrap_width > 0 ? options_.wrap_width : 40, 10, 72));
            out = pad + std::string(width > indent ? width - indent : 10, '-') + "\n";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out = "\f\n";
          }
        },
        block.node);
    return out;
  }

  std::string list(const ir::List& list, std::size_t indent, int depth) {
    std::string out;
    const std::string pad(indent, ' ');
    if (list.definition) {
      for (const ir::Blocks& item : list.items) {
        if (item.empty()) continue;
        if (!out.empty()) out.append("\n");
        if (const auto* term = item.front().get_if<ir::Paragraph>()) {
          out.append(wrap_text(inlines(term->content), width_for(indent), pad, pad)).append("\n");
        }
        ir::Blocks rest(item.begin() + 1, item.end());
        out.append(blocks(rest, indent + 4, depth + 1));
      }
      return out;
    }
    int number = list.start;
    const bool loose = list.loose;
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      std::string marker = list.ordered ? std::to_string(number++) + ". " : options_.bullet + " ";
      if (list.task(i) == ir::TaskState::Done) marker += "[x] ";
      if (list.task(i) == ir::TaskState::Open) marker += "[ ] ";
      const std::size_t inner_indent = indent + display_width(marker);
      std::string body = blocks(list.items[i], inner_indent, depth + 1);
      if (!loose) {
        // Tight lists: no blank lines inside items.
        body = util::replace_all(body, "\n\n", "\n");
      }
      if (body.size() >= inner_indent) {
        body.replace(0, inner_indent, pad + marker);
      } else {
        body = pad + marker + "\n";
      }
      if (!out.empty() && loose) out.append("\n");
      out.append(body);
    }
    return out;
  }

  std::string table(const ir::TableBlock& table, const std::string& pad) {
    const auto cells = table_strings(table, [this](const ir::Inlines& content) { return inlines(content); });
    std::string out;
    if (!table.caption.empty()) out.append(pad).append(inlines(table.caption)).append("\n");
    if (options_.ascii_tables) {
      GridTableStyle style;
      style.header_rule = '=';
      out.append(render_grid_table(cells, header_row_count(table), table.alignments, style, pad));
    } else {
      for (const auto& row : cells) {
        std::string line = pad;
        for (std::size_t c = 0; c < row.size(); ++c) {
          if (c > 0) line.push_back('\t');
          line.append(util::replace_all(row[c], "\n", " "));
        }
        out.append(line).append("\n");
      }
    }
    return out;
  }

  std::string inlines(const ir::Inlines& list, int depth = 0) {
    std::string out;
    if (depth > ir::max_document_depth * 2) return out;
    for (const ir::Inline& item : list) {
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            if constexpr (std::same_as<T, ir::Text>) {
              if (options_.wrap_width > 0) {
                // Soft line breaks are re-flowed by wrapping.
                std::string text = node.text;
                for (char& c : text) {
                  if (c == '\n') c = ' ';
                }
                out.append(text);
              } else {
                out.append(node.text);
              }
            } else if constexpr (std::same_as<T, ir::Code>) {
              out.append(node.raw_format.empty() ? node.text : std::string());
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              out.append("\n");
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              out.append("[").append(node.alt.empty() ? "Image" : node.alt).append("]");
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                const std::size_t index = footnote_index(doc_, node.note);
                if (index > 0) {
                  out.append("[").append(std::to_string(note_number(index))).append("]");
                  return;
                }
              }
              const std::string label = inlines(node.children, depth + 1);
              out.append(label);
              const bool internal = node.url.starts_with("#") || node.url.empty();
              if (internal || label == node.url || "mailto:" + label == node.url) return;
              if (options_.link_references) {
                const auto it = std::ranges::find_if(link_targets_, [&](const auto& e) { return e.second == node.url; });
                const int number = it != link_targets_.end() ? it->first : ++counter_;
                if (it == link_targets_.end()) link_targets_.emplace_back(number, node.url);
                out.append(" [").append(std::to_string(number)).append("]");
              } else {
                out.append(" <").append(node.url).append(">");
              }
            } else {
              out.append(inlines(node.children, depth + 1));
            }
          },
          item.node);
    }
    // Soft line breaks are re-flowed by wrapping; hard ones stay.
    return out;
  }

  const ir::Document& doc_;
  const PlainTextOptions& options_;
  std::map<std::size_t, int> note_numbers_;
  std::vector<std::pair<int, std::string>> link_targets_;
  int counter_ = 0;
};

// ---- inference ----------------------------------------------------------------------------

bool is_rule_line(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.size() < 3) return false;
  const char c = t.front();
  if (c != '-' && c != '*' && c != '=' && c != '_' && c != '~' && c != '#') return false;
  std::size_t count = 0;
  for (const char x : t) {
    if (x == c) {
      ++count;
    } else if (x != ' ') {
      return false;
    }
  }
  return count >= 3;
}

// Bullet or number marker: returns (marker length incl. following space, ordered, number).
struct Marker {
  std::size_t length = 0;
  bool ordered = false;
  int number = 1;
  ir::ListNumbering numbering = ir::ListNumbering::Decimal;
};

std::optional<Marker> list_marker(std::string_view line) {
  const std::string_view t = util::trim_left(line);
  for (const std::string_view bullet : {"- ", "* ", "+ ", "\xE2\x80\xA2 ", "\xE2\x97\xA6 ", "\xE2\x80\xA3 ", "\xC2\xB7 "}) {
    if (t.starts_with(bullet) && t.size() > bullet.size()) return Marker{bullet.size(), false, 1, ir::ListNumbering::Decimal};
  }
  std::size_t digits = 0;
  while (digits < t.size() && digits < 4 && util::is_ascii_digit(t[digits])) ++digits;
  if (digits > 0 && digits + 1 < t.size() && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ') {
    return Marker{digits + 2, true, static_cast<int>(util::parse_int(t.substr(0, digits)).value_or(1)),
                  ir::ListNumbering::Decimal};
  }
  if (t.size() > 3 && util::is_ascii_alpha(t[0]) && (t[1] == ')' || t[1] == '.') && t[2] == ' ' &&
      (t[0] == 'a' || t[0] == 'b' || t[0] == 'c' || t[0] == 'A' || t[0] == 'B' || t[0] == 'C')) {
    const bool upper = t[0] <= 'Z';
    return Marker{3, true, util::ascii_lower(t[0]) - 'a' + 1, upper ? ir::ListNumbering::UpperAlpha : ir::ListNumbering::LowerAlpha};
  }
  return std::nullopt;
}

bool looks_like_title(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.empty() || t.size() > 70) return false;
  std::size_t letters = 0;
  for (const char c : t) {
    if (c >= 'a' && c <= 'z') return false;  // not all caps
    if (c >= 'A' && c <= 'Z') ++letters;
  }
  const char last = t.back();
  return letters >= 3 && last != '.' && last != ',' && last != ';' && last != ':';
}

// "1 Introduction", "2.3 Scope" -> level by number of components.
int numbered_section_level(std::string_view line) {
  const std::string_view t = util::trim(line);
  std::size_t i = 0;
  int components = 0;
  while (i < t.size()) {
    std::size_t digits = 0;
    while (i + digits < t.size() && util::is_ascii_digit(t[i + digits])) ++digits;
    if (digits == 0 || digits > 3) return 0;
    ++components;
    i += digits;
    if (i < t.size() && t[i] == '.' && i + 1 < t.size() && util::is_ascii_digit(t[i + 1])) {
      ++i;
      continue;
    }
    break;
  }
  if (i < t.size() && t[i] == '.') ++i;
  if (i >= t.size() || t[i] != ' ' || components == 0 || components > 4) return 0;
  const std::string_view title = util::trim(t.substr(i));
  if (title.empty() || title.size() > 70 || title.back() == '.' || !util::is_ascii_alpha(title.front()) ||
      title.front() < 'A' || title.front() > 'Z') {
    return 0;
  }
  return components;
}

void infer_blocks(std::span<const std::string_view> lines, const TextInferenceOptions& options, ir::Blocks& out, int depth);

void infer_list(std::span<const std::string_view> lines, std::size_t& i, const TextInferenceOptions& options,
                ir::Blocks& out, int depth) {
  const Marker first = *list_marker(lines[i]);
  ir::List list;
  list.ordered = first.ordered;
  list.start = first.number;
  list.numbering = first.numbering;
  const std::size_t base_indent = indentation(lines[i]);
  bool blank_between = false;
  while (i < lines.size()) {
    const auto marker = list_marker(lines[i]);
    if (!marker || marker->ordered != first.ordered || indentation(lines[i]) != base_indent) break;
    const std::size_t content_indent = base_indent + marker->length;
    std::vector<std::string> item_lines;
    item_lines.emplace_back(util::trim_left(lines[i]).substr(marker->length));
    ++i;
    while (i < lines.size()) {
      if (is_blank(lines[i])) {
        // A blank line continues the item when indented content follows.
        if (i + 1 < lines.size() && !is_blank(lines[i + 1]) && indentation(lines[i + 1]) >= content_indent) {
          item_lines.emplace_back();
          ++i;
          blank_between = true;
          continue;
        }
        if (i + 1 < lines.size() && list_marker(lines[i + 1]) && indentation(lines[i + 1]) == base_indent) {
          blank_between = true;
          ++i;
        }
        break;
      }
      if (list_marker(lines[i]) && indentation(lines[i]) <= base_indent) break;
      if (indentation(lines[i]) > base_indent || !list_marker(lines[i])) {
        item_lines.emplace_back(strip_indent(lines[i], content_indent));
        ++i;
        continue;
      }
      break;
    }
    std::vector<std::string_view> views(item_lines.begin(), item_lines.end());
    ir::Blocks item;
    if (depth < 16) {
      infer_blocks(views, options, item, depth + 1);
    } else {
      item.push_back(ir::Paragraph{{ir::Text{join_lines(views)}}});
    }
    list.items.push_back(std::move(item));
  }
  list.loose = blank_between;
  out.push_back(std::move(list));
}

void infer_blocks(std::span<const std::string_view> lines, const TextInferenceOptions& options, ir::Blocks& out, int depth) {
  std::size_t i = 0;
  std::vector<std::string_view> paragraph;
  bool previous_blank = true;
  const auto flush = [&] {
    if (paragraph.empty()) return;
    std::string text;
    for (std::size_t k = 0; k < paragraph.size(); ++k) {
      if (k > 0) text.push_back('\n');
      text.append(util::trim(paragraph[k]));
    }
    out.push_back(ir::Paragraph{{ir::Text{std::move(text)}}});
    paragraph.clear();
  };
  while (i < lines.size()) {
    const std::string_view line = lines[i];
    if (is_blank(line)) {
      flush();
      previous_blank = true;
      ++i;
      continue;
    }
    const bool next_blank = i + 1 >= lines.size() || is_blank(lines[i + 1]);
    // Setext-style headings: a text line underlined with === or ---.
    if (options.headings && paragraph.empty() && i + 1 < lines.size() && !is_rule_line(line)) {
      const std::string_view under = util::trim(lines[i + 1]);
      if (under.size() >= 3 && (is_repeated(under, '=', 3) || is_repeated(under, '-', 3)) &&
          under.size() + 3 >= display_width(util::trim(line)) && display_width(util::trim(line)) <= 100) {
        out.push_back(ir::Heading{under.front() == '=' ? 1 : 2, {ir::Text{trim_copy(line)}}});
        i += 2;
        previous_blank = false;
        continue;
      }
    }
    if (options.rules && paragraph.empty() && is_rule_line(line)) {
      out.push_back(ir::HorizontalRule{});
      ++i;
      continue;
    }
    if (options.code && paragraph.empty() && previous_blank && indentation(line) >= 4) {
      std::vector<std::string> code;
      while (i < lines.size() && (indentation(lines[i]) >= 4 || is_blank(lines[i]))) {
        code.emplace_back(strip_indent(lines[i], 4));
        ++i;
      }
      while (!code.empty() && util::trim(code.back()).empty()) code.pop_back();
      out.push_back(ir::CodeBlock{{}, util::join(code, "\n")});
      previous_blank = false;
      continue;
    }
    if (options.quotes && paragraph.empty() && util::trim_left(line).starts_with(">")) {
      std::vector<std::string_view> quoted;
      while (i < lines.size() && util::trim_left(lines[i]).starts_with(">")) {
        std::string_view q = util::trim_left(lines[i]).substr(1);
        if (q.starts_with(" ")) q.remove_prefix(1);
        quoted.push_back(q);
        ++i;
      }
      ir::BlockQuote quote;
      if (depth < 16) {
        infer_blocks(quoted, options, quote.blocks, depth + 1);
      } else {
        quote.blocks.push_back(ir::Paragraph{{ir::Text{join_lines(quoted)}}});
      }
      out.push_back(std::move(quote));
      previous_blank = false;
      continue;
    }
    if (options.lists && paragraph.empty() && list_marker(line)) {
      infer_list(lines, i, options, out, depth);
      previous_blank = false;
      continue;
    }
    if (options.headings && paragraph.empty() && previous_blank && next_blank) {
      if (const int level = numbered_section_level(line); level > 0) {
        out.push_back(ir::Heading{std::min(level, 6), {ir::Text{trim_copy(line)}}});
        ++i;
        previous_blank = false;
        continue;
      }
      if (looks_like_title(line)) {
        out.push_back(ir::Heading{out.empty() ? 1 : 2, {ir::Text{trim_copy(line)}}});
        ++i;
        previous_blank = false;
        continue;
      }
    }
    paragraph.push_back(line);
    previous_blank = false;
    ++i;
  }
  flush();
}

}  // namespace

std::string to_plain_text(const ir::Document& document, const PlainTextOptions& options) {
  TextRenderer renderer(document, options);
  return renderer.run();
}

ir::Document infer_document_from_text(std::string_view text, const TextInferenceOptions& options) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  const std::vector<std::string_view> lines = util::split_lines(text);
  ir::Document document;
  infer_blocks(lines, options, document.blocks, 0);
  return document;
}

}  // namespace cvt::document
