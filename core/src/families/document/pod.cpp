// Convertible core - document family: Perl POD (Plain Old Documentation).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/html_internal.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

constexpr int max_code_depth = 24;

// ---- formatting codes: B<..> I<..> C<..> L<..> E<..> F<..> S<..> X<..> Z<..> ----

std::string pod_entity(std::string_view name) {
  if (name == "lt") return "<";
  if (name == "gt") return ">";
  if (name == "verbar") return "|";
  if (name == "sol") return "/";
  if (!name.empty() && util::is_ascii_digit(name.front())) {
    const auto value = name.starts_with("0x") || name.starts_with("0X") ? util::parse_int(name.substr(2), 16)
                       : name.starts_with("0")                          ? util::parse_int(name, 8)
                                                                        : util::parse_int(name);
    std::string out;
    if (value && *value > 0 && *value <= 0x10FFFF) util::append_utf8(out, static_cast<char32_t>(*value));
    return out;
  }
  const std::string_view value = html::lookup_named_reference(std::string(name) + ";");
  return value.empty() ? "E<" + std::string(name) + ">" : std::string(value);
}

// Parses formatting codes; returns the inlines and advances `pos` to after the
// closing delimiter (or the end).
ir::Inlines parse_codes(std::string_view text, std::size_t& pos, std::string_view close, int depth);

ir::Inlines code_node(char code, std::string_view inner_text, ir::Inlines inner) {
  switch (code) {
    case 'B': return {ir::Strong{std::move(inner)}};
    case 'I': return {ir::Emphasis{std::move(inner)}};
    case 'C': return {ir::Code{ir::plain_text(inner)}};
    case 'F': return {ir::Emphasis{std::move(inner)}};
    case 'S': {
      std::string flat = ir::plain_text(inner);
      flat = util::replace_all(flat, " ", "\xC2\xA0");
      return {ir::Text{flat}};
    }
    case 'X':
    case 'Z': return {};
    case 'L': {
      // L<text|target>, L<target>, L<"section">, L<name/"section">
      const std::string raw(inner_text);
      std::string label;
      std::string target = raw;
      const std::size_t bar = raw.find('|');
      if (bar != std::string::npos) {
        label = raw.substr(0, bar);
        target = raw.substr(bar + 1);
      }
      std::string url = target;
      if (target.find("://") == std::string::npos && !target.starts_with("mailto:")) {
        // A man page or section reference.
        url = target.starts_with("\"") ? "#" + slugify(target.substr(1, target.size() - 2)) : target;
      }
      ir::Inlines kids;
      if (!label.empty()) {
        std::size_t p = 0;
        kids = parse_codes(label, p, {}, 1);
      } else {
        kids = {ir::Text{target}};
      }
      return {ir::Link{url, {}, std::move(kids)}};
    }
    default: return inner;
  }
}

ir::Inlines parse_codes(std::string_view text, std::size_t& pos, std::string_view close, int depth) {
  ir::Inlines out;
  std::size_t literal = pos;
  const auto flush = [&](std::size_t end) {
    if (end > literal) append_text(out, text.substr(literal, end - literal));
  };
  while (pos < text.size()) {
    if (!close.empty() && text.substr(pos).starts_with(close)) {
      // A closing ">>" must be preceded by whitespace.
      if (close.size() == 1 || (pos > 0 && util::is_ascii_space(text[pos - 1]))) {
        flush(close.size() > 1 ? pos - 1 : pos);
        pos += close.size();
        return out;
      }
    }
    const char c = text[pos];
    if (c >= 'A' && c <= 'Z' && pos + 1 < text.size() && text[pos + 1] == '<' && depth < max_code_depth) {
      flush(pos);
      std::size_t brackets = 1;
      while (pos + 1 + brackets < text.size() && text[pos + 1 + brackets] == '<') ++brackets;
      std::string inner_close;
      std::size_t start = pos + 2;
      if (brackets >= 2 && pos + 1 + brackets < text.size() && util::is_ascii_space(text[pos + 1 + brackets])) {
        inner_close = std::string(brackets, '>');
        start = pos + 1 + brackets + 1;
      } else {
        inner_close = ">";
      }
      if (c == 'E') {
        const std::size_t end = text.find('>', start);
        if (end != std::string_view::npos) {
          append_text(out, pod_entity(text.substr(start, end - start)));
          pos = end + 1;
          literal = pos;
          continue;
        }
      }
      const std::size_t inner_start = start;
      std::size_t p = start;
      ir::Inlines inner = parse_codes(text, p, inner_close, depth + 1);
      std::size_t inner_end = p >= inner_close.size() ? p - inner_close.size() : p;
      if (inner_close.size() > 1 && inner_end > inner_start) --inner_end;  // the space before ">>"
      std::string inner_text(text.substr(inner_start, inner_end > inner_start ? inner_end - inner_start : 0));
      for (ir::Inline& node : code_node(c, inner_text, std::move(inner))) out.push_back(std::move(node));
      pos = p;
      literal = pos;
      continue;
    }
    ++pos;
  }
  flush(pos);
  return out;
}

ir::Inlines pod_inlines(std::string_view text) {
  std::size_t pos = 0;
  ir::Inlines out = parse_codes(text, pos, {}, 0);
  return out;
}

// ---- paragraphs -------------------------------------------------------------------------------

struct PodParagraph {
  std::string command;  // "head1", "over", "item", ... empty for ordinary/verbatim
  std::string text;
  bool verbatim = false;
};

std::vector<PodParagraph> split_paragraphs(std::string_view text) {
  std::vector<PodParagraph> out;
  const std::vector<std::string_view> lines = util::split_lines(text);
  bool in_pod = false;
  std::size_t i = 0;
  // Files that start with prose (no =pod) are treated as POD as well.
  for (const std::string_view l : lines) {
    if (!is_blank(l)) {
      in_pod = l.starts_with("=");
      break;
    }
  }
  if (!in_pod) in_pod = true;
  while (i < lines.size()) {
    if (is_blank(lines[i])) {
      ++i;
      continue;
    }
    std::vector<std::string_view> block;
    while (i < lines.size() && !is_blank(lines[i])) block.push_back(lines[i++]);
    const std::string_view first = block.front();
    if (first.starts_with("=")) {
      std::size_t end = 1;
      while (end < first.size() && (util::is_ascii_alpha(first[end]) || util::is_ascii_digit(first[end]))) ++end;
      PodParagraph p;
      p.command = std::string(first.substr(1, end - 1));
      std::string rest(util::trim(first.substr(end)));
      for (std::size_t k = 1; k < block.size(); ++k) rest.append("\n").append(block[k]);
      p.text = std::move(rest);
      if (p.command == "cut") {
        in_pod = false;
        continue;
      }
      in_pod = true;
      out.push_back(std::move(p));
      continue;
    }
    if (!in_pod) continue;
    PodParagraph p;
    p.verbatim = first.starts_with(" ") || first.starts_with("\t");
    p.text = join_lines(block);
    out.push_back(std::move(p));
  }
  return out;
}

struct ListFrame {
  ir::List list;
  bool started = false;
};

ir::Document read_pod(std::string_view text) {
  ir::Document document;
  const std::vector<PodParagraph> paragraphs = split_paragraphs(text);
  std::vector<ListFrame> lists;  // =over nesting
  std::vector<ir::Blocks> targets(1);
  bool in_name_section = false;
  bool skipping = false;  // =begin non-pod formats
  std::string begin_format;
  const auto current = [&]() -> ir::Blocks& {
    if (!lists.empty() && !lists.back().list.items.empty()) return lists.back().list.items.back();
    return targets.back();
  };
  for (const PodParagraph& p : paragraphs) {
    if (skipping) {
      if (p.command == "end") skipping = false;
      continue;
    }
    if (p.command.starts_with("head") && p.command.size() == 5) {
      while (!lists.empty()) {
        ListFrame frame = std::move(lists.back());
        lists.pop_back();
        if (!frame.list.items.empty()) current().push_back(std::move(frame.list));
      }
      const int level = std::clamp(p.command[4] - '0', 1, 6);
      ir::Inlines content = pod_inlines(p.text);
      in_name_section = level == 1 && util::to_upper(ir::plain_text(content)) == "NAME";
      targets.back().push_back(ir::Heading{level, std::move(content)});
      continue;
    }
    if (p.command == "over") {
      if (lists.size() < 16) lists.push_back({});
      continue;
    }
    if (p.command == "back") {
      if (lists.empty()) continue;
      ListFrame frame = std::move(lists.back());
      lists.pop_back();
      if (frame.list.items.empty()) continue;
      current().push_back(std::move(frame.list));
      continue;
    }
    if (p.command == "item") {
      if (lists.empty()) lists.push_back({});
      ListFrame& frame = lists.back();
      std::string_view label = util::trim(p.text);
      if (!frame.started) {
        frame.started = true;
        if (label == "*" || label.starts_with("* ") || label.empty()) {
          frame.list.ordered = false;
        } else if (!label.empty() && util::is_ascii_digit(label.front())) {
          frame.list.ordered = true;
          frame.list.start = static_cast<int>(util::parse_int(label.substr(0, label.find_first_not_of("0123456789"))).value_or(1));
        } else {
          frame.list.definition = true;
        }
      }
      ir::Blocks item;
      if (frame.list.definition) {
        item.push_back(ir::Paragraph{pod_inlines(label)});
      } else {
        std::string_view rest = label;
        if (frame.list.ordered) {
          rest = rest.substr(std::min(rest.size(), rest.find_first_not_of("0123456789")));
          if (rest.starts_with(".")) rest.remove_prefix(1);
        } else if (rest.starts_with("*")) {
          rest.remove_prefix(1);
        }
        rest = util::trim(rest);
        if (!rest.empty()) item.push_back(ir::Paragraph{pod_inlines(rest)});
      }
      frame.list.items.push_back(std::move(item));
      continue;
    }
    if (p.command == "begin") {
      begin_format = util::to_lower(util::trim(p.text));
      if (begin_format != "pod" && !begin_format.starts_with(":")) skipping = true;
      continue;
    }
    if (p.command == "for") {
      const std::string_view t = util::trim(p.text);
      const std::size_t space = t.find_first_of(" \n");
      const std::string format = util::to_lower(t.substr(0, space));
      if (format == "html" && space != std::string_view::npos) {
        ir::CodeBlock raw{{}, std::string(t.substr(space + 1))};
        raw.raw_format = "html";
        current().push_back(std::move(raw));
      }
      continue;
    }
    if (!p.command.empty()) continue;  // =pod, =encoding, =end, unknown
    if (p.verbatim) {
      // Remove the common indentation.
      const std::vector<std::string_view> lines = util::split_lines(p.text);
      std::size_t common = std::string::npos;
      for (const std::string_view l : lines) {
        if (!is_blank(l)) common = std::min(common, indentation(l));
      }
      std::vector<std::string> stripped;
      for (const std::string_view l : lines) stripped.emplace_back(strip_indent(l, common == std::string::npos ? 0 : common));
      // Consecutive verbatim paragraphs form one code block.
      ir::Blocks& target = current();
      if (!target.empty()) {
        if (auto* code = target.back().get_if<ir::CodeBlock>(); code != nullptr && code->raw_format.empty()) {
          code->code.append("\n\n").append(util::join(stripped, "\n"));
          continue;
        }
      }
      target.push_back(ir::CodeBlock{{}, util::join(stripped, "\n")});
      continue;
    }
    ir::Inlines content = pod_inlines(p.text);
    if (in_name_section && document.metadata.title.empty()) {
      const std::string name = inline_line(content);
      document.metadata.title = std::string(util::trim(name.substr(0, name.find(" - "))));
    }
    current().push_back(ir::Paragraph{std::move(content)});
  }
  while (!lists.empty()) {
    ListFrame frame = std::move(lists.back());
    lists.pop_back();
    if (!frame.list.items.empty()) current().push_back(std::move(frame.list));
  }
  document.blocks = std::move(targets.front());
  return document;
}

// ---- writing ----------------------------------------------------------------------------------

std::string pod_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '<') {
      out.append("E<lt>");
    } else if (c == '>') {
      out.append("E<gt>");
    } else if (c == '\n') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Inside a formatting code, '|' and '/' are significant in L<>.
std::string pod_link_escape(std::string_view text) {
  std::string out = pod_escape(text);
  out = util::replace_all(out, "|", "E<verbar>");
  out = util::replace_all(out, "/", "E<sol>");
  return out;
}

InlineMarkup pod_markup() {
  InlineMarkup m;
  m.escape = pod_escape;
  m.strong = {"B<", ">"};
  m.emphasis = {"I<", ">"};
  m.soft_break = " ";
  m.line_break = "\n";
  m.code = [](const ir::Code& code) -> std::string {
    if (!code.raw_format.empty()) return {};
    if (code.text.empty()) return {};
    return "C<" + pod_escape(code.text) + ">";
  };
  m.link = [](const ir::Link& link, const std::string& label) -> std::string {
    std::string target = link.url;
    std::erase_if(target, [](char c) { return c == '>' || c == '<' || util::is_ascii_space(c); });
    if (target.empty()) return label;
    if (target.find("://") == std::string::npos && !target.starts_with("mailto:")) {
      // POD only links text labels to URLs; other targets keep the label.
      return label;
    }
    std::string plain = pod_link_escape(ir::plain_text(link.children));
    if (plain == target) return "L<" + target + ">";
    return "L<" + plain + "|" + target + ">";
  };
  m.image = [](const ir::InlineImage& image) { return pod_escape(image.alt); };
  m.note = [](const ir::Link&, std::size_t index) { return "[" + std::to_string(index) + "]"; };
  return m;
}

class PodWriter {
 public:
  explicit PodWriter(const ir::Document& document) : doc_(document), markup_(pod_markup()) {}

  std::string run() {
    std::string out = "=pod\n\n";
    out.append(blocks(doc_.blocks, 0));
    if (!doc_.footnotes.empty()) {
      out.append("=head2 Notes\n\n=over 4\n\n");
      for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
        out.append("=item [").append(std::to_string(i + 1)).append("]\n\n").append(blocks(doc_.footnotes[i].blocks, 1));
      }
      out.append("=back\n\n");
    }
    out.append("=cut\n");
    return out;
  }

 private:
  std::string paragraph_text(const ir::Inlines& content) {
    std::string text = render_inlines(content, markup_, &doc_);
    // Lines must not start with whitespace (verbatim) or '=' (command).
    std::string out;
    bool first = true;
    for (std::string_view line : util::split(text, '\n')) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
      if (!first) out.push_back('\n');
      if (line.starts_with("=")) out.append("Z<>");
      out.append(line);
      first = false;
    }
    return out;
  }

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    for (const ir::Block& b : list) out.append(block(b, depth));
    return out;
  }

  static std::string verbatim(std::string_view code) {
    std::string out;
    for (const std::string_view line : util::split(code, '\n')) {
      out.append(line.empty() ? "" : "    ").append(line).append("\n");
    }
    return out + "\n";
  }

  std::string block(const ir::Block& block, int depth) {
    if (depth > ir::max_document_depth * 2) return {};
    std::string out;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            const std::string text = paragraph_text(node.content);
            if (!text.empty()) out = text + "\n\n";
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = render_inlines(node.content, markup_, &doc_);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            out = "=head" + std::to_string(std::clamp(node.level, 1, 4)) + " " + std::string(util::trim(text)) + "\n\n";
          } else if constexpr (std::same_as<T, ir::List>) {
            out = "=over 4\n\n";
            int number = node.start;
            for (std::size_t i = 0; i < node.items.size(); ++i) {
              const ir::Blocks& item = node.items[i];
              std::size_t first_block = 0;
              if (node.definition) {
                std::string term;
                if (!item.empty()) {
                  if (const auto* p = item.front().get_if<ir::Paragraph>()) term = paragraph_text(p->content);
                }
                for (char& c : term) {
                  if (c == '\n') c = ' ';
                }
                out.append("=item ").append(term.empty() ? "Z<>" : term).append("\n\n");
                first_block = 1;
              } else {
                out.append(node.ordered ? "=item " + std::to_string(number++) + "." : std::string("=item *")).append("\n\n");
              }
              for (std::size_t b = first_block; b < item.size(); ++b) out.append(this->block(item[b], depth + 1));
            }
            out.append("=back\n\n");
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty()) {
              if (node.raw_format == "html") out = "=begin html\n\n" + node.code + "\n\n=end html\n\n";
              return;
            }
            out = verbatim(node.code);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            out = "=over 4\n\n" + blocks(node.blocks, depth + 1) + "=back\n\n";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            const auto cells = table_strings(node, [](const ir::Inlines& c) { return inline_line(c); });
            out = verbatim(render_grid_table(cells, header_row_count(node), node.alignments, GridTableStyle{}));
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string& text = node.caption.empty() ? node.alt : node.caption;
            if (!text.empty()) out = pod_escape(text) + "\n\n";
          } else if constexpr (std::same_as<T, ir::HorizontalRule> || std::same_as<T, ir::PageBreak>) {
            out.clear();
          }
        },
        block.node);
    return out;
  }

  const ir::Document& doc_;
  InlineMarkup markup_;
};

int sniff_pod(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"pod"})) return 85;
  const std::string_view head = sniff_head(data);
  if (head.starts_with("=pod") || head.starts_with("=head1") || head.starts_with("=encoding")) return 75;
  if (has_extension(filename, {"pm", "pl"}) && head.find("\n=head1") != std::string_view::npos) return 40;
  return 0;
}

}  // namespace

void register_pod(Registry& registry) {
  MarkupFormat pod;
  pod.id = "pod";
  pod.name = "Perl POD";
  pod.description = "Plain Old Documentation, the markup of Perl documentation.";
  pod.extensions = {"pod"};
  pod.mime = "text/x-pod";
  pod.popularity = 10;
  pod.sniff = sniff_pod;
  pod.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_pod(text); };
  pod.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    PodWriter writer(document);
    return writer.run();
  };
  pod.write_lossy = true;  // no tables (written as text), no pictures, no strike/underline
  register_markup(registry, std::move(pod));
}

}  // namespace cvt::document
