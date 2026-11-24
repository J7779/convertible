// Convertible core - document family: BBCode (forum markup).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

constexpr int max_nesting = 48;

// ---- tokenizer and tree -----------------------------------------------------------------

struct Node {
  std::string tag;       // lower-case; empty for text
  std::string argument;  // [tag=argument]
  std::string text;      // text nodes
  std::vector<Node> children;
};

bool known_tag(std::string_view tag) {
  static constexpr std::array<std::string_view, 40> tags = {
      "b",     "i",    "u",     "s",     "strike", "sup",   "sub",   "url",   "img",    "code",
      "quote", "list", "*",     "li",    "table",  "tr",    "th",    "td",    "h1",     "h2",
      "h3",    "h4",   "h5",    "h6",    "hr",     "size",  "color", "font",  "center", "left",
      "right", "email", "spoiler", "noparse", "ul", "ol",   "pre",   "justify", "indent", "highlight"};
  return std::ranges::find(tags, tag) != tags.end();
}

bool verbatim_tag(std::string_view tag) { return tag == "code" || tag == "noparse" || tag == "pre"; }

Node parse_tree(std::string_view text) {
  Node root;
  std::vector<Node*> stack{&root};
  std::size_t i = 0;
  std::size_t literal = 0;
  const auto flush = [&](std::size_t end) {
    if (end > literal) {
      Node t;
      t.text = std::string(text.substr(literal, end - literal));
      stack.back()->children.push_back(std::move(t));
    }
  };
  while (i < text.size()) {
    if (text[i] != '[') {
      ++i;
      continue;
    }
    const std::size_t close = text.find(']', i);
    if (close == std::string_view::npos || close - i > 2048) {
      ++i;
      continue;
    }
    std::string_view inside = text.substr(i + 1, close - i - 1);
    const bool closing = inside.starts_with("/");
    if (closing) inside.remove_prefix(1);
    std::size_t name_end = 0;
    while (name_end < inside.size() && (util::is_ascii_alpha(inside[name_end]) || util::is_ascii_digit(inside[name_end]) ||
                                        inside[name_end] == '*')) {
      ++name_end;
    }
    const std::string name = util::to_lower(inside.substr(0, name_end));
    if (!known_tag(name)) {
      ++i;
      continue;
    }
    flush(i);
    if (closing) {
      // Close the nearest matching open tag (and everything inside it).
      std::size_t k = stack.size();
      while (k > 1 && stack[k - 1]->tag != name && !(name == "list" && stack[k - 1]->tag == "*")) --k;
      if (name == "list") {
        while (k > 1 && stack[k - 1]->tag != "list" && stack[k - 1]->tag != "ul" && stack[k - 1]->tag != "ol") --k;
      }
      if (k > 1) stack.resize(k - 1);
      i = close + 1;
      literal = i;
      continue;
    }
    Node node;
    node.tag = name;
    std::string_view argument = inside.substr(name_end);
    if (argument.starts_with("=")) {
      argument.remove_prefix(1);
      if (argument.size() >= 2 && (argument.front() == '"' || argument.front() == '\'') && argument.back() == argument.front()) {
        argument = argument.substr(1, argument.size() - 2);
      }
      node.argument = std::string(argument);
    } else if (!util::trim(argument).empty()) {
      // [img alt="..."] style attributes.
      node.argument = std::string(util::trim(argument));
    }
    if (verbatim_tag(name)) {
      const std::string end_tag = "[/" + name + "]";
      const std::size_t end = util::to_lower(text.substr(close + 1)).find(end_tag);
      const std::size_t stop = end == std::string::npos ? text.size() : close + 1 + end;
      Node content;
      content.text = std::string(text.substr(close + 1, stop - close - 1));
      node.children.push_back(std::move(content));
      stack.back()->children.push_back(std::move(node));
      i = end == std::string::npos ? text.size() : stop + end_tag.size();
      literal = i;
      continue;
    }
    if (name == "*" || name == "li") {
      // A new item closes the previous one.
      if (stack.back()->tag == "*" || stack.back()->tag == "li") stack.pop_back();
    }
    if (name == "hr") {
      stack.back()->children.push_back(std::move(node));
      i = close + 1;
      literal = i;
      continue;
    }
    stack.back()->children.push_back(std::move(node));
    if (stack.size() < max_nesting) stack.push_back(&stack.back()->children.back());
    i = close + 1;
    literal = i;
  }
  flush(text.size());
  return root;
}

// ---- tree -> IR ---------------------------------------------------------------------------

bool is_block_tag(std::string_view tag) {
  return tag == "quote" || tag == "code" || tag == "list" || tag == "ul" || tag == "ol" || tag == "table" ||
         tag == "hr" || tag == "center" || tag == "left" || tag == "right" || tag == "justify" || tag == "indent" ||
         tag == "spoiler" || tag == "pre" || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}

class Converter {
 public:
  void inline_nodes(const std::vector<Node>& nodes, ir::Inlines& out, int depth) {
    for (const Node& n : nodes) inline_node(n, out, depth);
  }

  void inline_node(const Node& n, ir::Inlines& out, int depth) {
    if (n.tag.empty()) {
      // Newlines inside inline content are hard breaks in BBCode.
      std::size_t start = 0;
      for (std::size_t nl = n.text.find('\n'); nl != std::string::npos; nl = n.text.find('\n', start)) {
        append_text(out, std::string_view(n.text).substr(start, nl - start));
        out.push_back(ir::LineBreak{});
        start = nl + 1;
      }
      append_text(out, std::string_view(n.text).substr(start));
      return;
    }
    if (depth > max_nesting) return;
    ir::Inlines kids;
    const auto children = [&] { inline_nodes(n.children, kids, depth + 1); };
    const std::string& t = n.tag;
    if (t == "b") {
      children();
      out.push_back(ir::Strong{std::move(kids)});
    } else if (t == "i") {
      children();
      out.push_back(ir::Emphasis{std::move(kids)});
    } else if (t == "u") {
      children();
      out.push_back(ir::Underline{std::move(kids)});
    } else if (t == "s" || t == "strike") {
      children();
      out.push_back(ir::Strikethrough{std::move(kids)});
    } else if (t == "sup") {
      children();
      out.push_back(ir::Superscript{std::move(kids)});
    } else if (t == "sub") {
      children();
      out.push_back(ir::Subscript{std::move(kids)});
    } else if (t == "highlight") {
      children();
      ir::Attributes attr;
      attr.add_class("mark");
      out.push_back(ir::Inline(ir::Strong{std::move(kids)}, std::move(attr)));
    } else if (t == "url" || t == "email") {
      children();
      std::string url = n.argument;
      if (url.empty()) url = ir::plain_text(kids);
      if (t == "email" && !url.starts_with("mailto:")) url = "mailto:" + url;
      out.push_back(ir::Link{std::string(util::trim(url)), {}, std::move(kids)});
    } else if (t == "img") {
      std::string src;
      for (const Node& c : n.children) src += c.text;
      std::string alt;
      if (const std::size_t at = n.argument.find("alt="); at != std::string::npos) {
        std::string_view rest = std::string_view(n.argument).substr(at + 4);
        if (rest.starts_with("\"")) {
          rest.remove_prefix(1);
          alt = std::string(rest.substr(0, rest.find('"')));
        } else {
          alt = std::string(rest.substr(0, rest.find(' ')));
        }
      }
      out.push_back(ir::InlineImage{wiki::image_from_url(util::trim(src)), alt});
    } else if (t == "noparse") {
      for (const Node& c : n.children) append_text(out, c.text);
    } else if (t == "code" || t == "pre") {
      std::string code;
      for (const Node& c : n.children) code += c.text;
      out.push_back(ir::Code{std::move(code)});
    } else {
      // size, color, font and unknown inline wrappers are transparent.
      children();
      for (ir::Inline& k : kids) out.push_back(std::move(k));
    }
  }

  // Splits a node sequence into blocks: block tags stand alone, the rest forms
  // paragraphs separated by blank lines.
  ir::Blocks blocks(const std::vector<Node>& nodes, int depth) {
    ir::Blocks out;
    std::vector<const Node*> run;
    const auto flush = [&] {
      if (run.empty()) return;
      // Split text at blank lines.
      std::vector<Node> pieces;
      pieces.emplace_back();
      for (const Node* n : run) {
        if (n->tag.empty()) {
          std::size_t start = 0;
          for (std::size_t gap = n->text.find("\n\n"); gap != std::string::npos; gap = n->text.find("\n\n", start)) {
            Node piece;
            piece.text = n->text.substr(start, gap - start);
            pieces.back().children.push_back(std::move(piece));
            pieces.emplace_back();
            start = gap + 2;
            while (start < n->text.size() && n->text[start] == '\n') ++start;
          }
          Node piece;
          piece.text = n->text.substr(start);
          pieces.back().children.push_back(std::move(piece));
        } else {
          pieces.back().children.push_back(*n);
        }
      }
      for (Node& piece : pieces) {
        // Trim newlines at the edges of the paragraph.
        while (!piece.children.empty() && piece.children.front().tag.empty() &&
               util::trim(piece.children.front().text).empty()) {
          piece.children.erase(piece.children.begin());
        }
        if (!piece.children.empty() && piece.children.front().tag.empty()) {
          std::string& s = piece.children.front().text;
          s.erase(0, s.find_first_not_of("\r\n"));
        }
        while (!piece.children.empty() && piece.children.back().tag.empty() && util::trim(piece.children.back().text).empty()) {
          piece.children.pop_back();
        }
        if (!piece.children.empty() && piece.children.back().tag.empty()) {
          std::string& s = piece.children.back().text;
          while (s.ends_with('\n') || s.ends_with('\r')) s.pop_back();
        }
        if (piece.children.empty()) continue;
        out.push_back(paragraph(piece.children, depth));
      }
      run.clear();
    };
    for (const Node& n : nodes) {
      if (is_block_tag(n.tag)) {
        flush();
        block(n, out, depth);
      } else {
        run.push_back(&n);
      }
    }
    flush();
    return out;
  }

  // A paragraph; one that is only a bold/underlined line is the heading convention
  // ([u][b]..[/b][/u] level 1, [b]..[/b] level 2, [u]..[/u] level 3).
  ir::Block paragraph(const std::vector<Node>& nodes, int depth) {
    if (nodes.size() == 1 && nodes[0].tag.empty()) {
      const std::string_view t = util::trim(nodes[0].text);
      if (t == "* * *" || t == "---" || t == "***" || t == "- - -") return ir::HorizontalRule{};
    }
    if (nodes.size() == 1 && (nodes[0].tag == "b" || nodes[0].tag == "u")) {
      const Node& n = nodes[0];
      const bool single_line = std::ranges::none_of(n.children, [](const Node& c) {
        return c.tag.empty() && c.text.find('\n') != std::string::npos;
      });
      if (single_line && !n.children.empty()) {
        int level = n.tag == "b" ? 2 : 3;
        const std::vector<Node>* content = &n.children;
        if (n.tag == "u" && n.children.size() == 1 && n.children[0].tag == "b") {
          level = 1;
          content = &n.children[0].children;
        }
        ir::Inlines text;
        inline_nodes(*content, text, depth + 1);
        return ir::Heading{level, std::move(text)};
      }
    }
    ir::Inlines content;
    inline_nodes(nodes, content, depth + 1);
    if (content.size() == 1) {
      if (auto* image = content.front().get_if<ir::InlineImage>()) {
        return ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, {}};
      }
    }
    return ir::Paragraph{std::move(content)};
  }

  void block(const Node& n, ir::Blocks& out, int depth) {
    if (depth > max_nesting) return;
    const std::string& t = n.tag;
    if (t == "hr") {
      out.push_back(ir::HorizontalRule{});
    } else if (t == "code" || t == "pre") {
      std::string code;
      for (const Node& c : n.children) code += c.text;
      if (code.starts_with("\n")) code.erase(0, 1);
      while (code.ends_with('\n')) code.pop_back();
      out.push_back(ir::CodeBlock{n.argument, std::move(code)});
    } else if (t == "quote" || t == "spoiler") {
      out.push_back(ir::BlockQuote{blocks(n.children, depth + 1)});
    } else if (t == "list" || t == "ul" || t == "ol") {
      ir::List list;
      list.ordered = t == "ol" || (!n.argument.empty() && n.argument != "disc" && n.argument != "circle" && n.argument != "square");
      if (list.ordered && !n.argument.empty()) {
        if (const auto start = util::parse_int(n.argument)) list.start = static_cast<int>(std::clamp<std::int64_t>(*start, -1000000, 1000000));
        if (n.argument == "a") list.numbering = ir::ListNumbering::LowerAlpha;
        if (n.argument == "A") list.numbering = ir::ListNumbering::UpperAlpha;
        if (n.argument == "i") list.numbering = ir::ListNumbering::LowerRoman;
        if (n.argument == "I") list.numbering = ir::ListNumbering::UpperRoman;
      }
      for (const Node& c : n.children) {
        if (c.tag == "*" || c.tag == "li") {
          ir::Blocks item = blocks(c.children, depth + 1);
          list.items.push_back(std::move(item));
        } else if (is_block_tag(c.tag) && !list.items.empty()) {
          block(c, list.items.back(), depth + 1);
        }
      }
      if (!list.items.empty()) out.push_back(std::move(list));
    } else if (t == "table") {
      ir::TableBlock table;
      for (const Node& row : n.children) {
        if (row.tag != "tr") continue;
        ir::TableRow r;
        bool all_header = true;
        for (const Node& cell : row.children) {
          if (cell.tag != "td" && cell.tag != "th") continue;
          if (cell.tag != "th") all_header = false;
          ir::Inlines content;
          inline_nodes(cell.children, content, depth + 1);
          r.cells.push_back(ir::TableCell{std::move(content), 1, 1});
        }
        r.header = all_header && !r.cells.empty() && (table.rows.empty() || table.rows.back().header);
        if (!r.cells.empty()) table.rows.push_back(std::move(r));
      }
      if (!table.rows.empty()) out.push_back(std::move(table));
    } else if (t.size() == 2 && t[0] == 'h') {
      ir::Inlines content;
      inline_nodes(n.children, content, depth + 1);
      out.push_back(ir::Heading{t[1] - '0', std::move(content)});
    } else {
      // center/left/right/indent: blocks inside.
      for (ir::Block& b : blocks(n.children, depth + 1)) out.push_back(std::move(b));
    }
  }
};

ir::Document read_bbcode(std::string_view text) {
  const std::string normalized = util::convert_line_endings(text, util::LineEnding::Lf);
  const Node root = parse_tree(normalized);
  Converter converter;
  ir::Document document;
  document.blocks = converter.blocks(root.children, 0);
  return document;
}

// ---- writing --------------------------------------------------------------------------------

std::string bb_escape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '[') {
      // A bracket that could start a tag is protected.
      const std::size_t close = text.find(']', i);
      std::size_t end = i + 1;
      if (end < text.size() && text[end] == '/') ++end;
      std::size_t name = end;
      while (name < text.size() && (util::is_ascii_alpha(text[name]) || text[name] == '*' || util::is_ascii_digit(text[name]))) ++name;
      if (close != std::string_view::npos && known_tag(util::to_lower(text.substr(end, name - end)))) {
        out.append("[noparse][[/noparse]");
        continue;
      }
    }
    out.push_back(text[i] == '\n' ? ' ' : text[i]);
  }
  return out;
}

class BbWriter {
 public:
  explicit BbWriter(const ir::Document& document) : doc_(document) {
    markup_.escape = bb_escape;
    markup_.strong = {"[b]", "[/b]"};
    markup_.emphasis = {"[i]", "[/i]"};
    markup_.underline = {"[u]", "[/u]"};
    markup_.strikethrough = {"[s]", "[/s]"};
    markup_.superscript = {"[sup]", "[/sup]"};
    markup_.subscript = {"[sub]", "[/sub]"};
    markup_.line_break = "\n";
    markup_.soft_break = " ";
    markup_.code = [](const ir::Code& code) -> std::string {
      if (!code.raw_format.empty()) return code.raw_format == "bbcode" ? code.text : std::string();
      std::string text = code.text;
      for (char& c : text) {
        if (c == '\n') c = ' ';
      }
      return "[noparse]" + util::replace_all(text, "[/noparse]", "[/ noparse]") + "[/noparse]";
    };
    markup_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      std::string url = link.url;
      std::erase_if(url, [](char c) { return c == ']' || c == '[' || util::is_ascii_space(c); });
      if (label == bb_escape(link.url)) return "[url]" + url + "[/url]";
      return "[url=" + url + "]" + label + "[/url]";
    };
    markup_.image = [](const ir::InlineImage& image) -> std::string {
      const std::string url = image_url(image.source, false);
      if (url.empty()) return bb_escape(image.alt);
      std::string alt = image.alt;
      std::erase_if(alt, [](char c) { return c == '"' || c == ']'; });
      return "[img" + (alt.empty() ? std::string() : " alt=\"" + alt + "\"") + "]" + url + "[/img]";
    };
    markup_.note = [](const ir::Link&, std::size_t index) { return "[sup]" + std::to_string(index) + "[/sup]"; };
  }

  std::string run() {
    std::string out = blocks(doc_.blocks, 0);
    if (!doc_.footnotes.empty()) {
      out.append("\n\n[hr]\n");
      for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
        out.append("\n[sup]").append(std::to_string(i + 1)).append("[/sup] ");
        out.append(util::replace_all(blocks(doc_.footnotes[i].blocks, 1), "\n\n", "\n"));
      }
    }
    while (out.ends_with('\n')) out.pop_back();
    return out + "\n";
  }

 private:
  std::string inlines(const ir::Inlines& content) { return render_inlines(content, markup_, &doc_); }

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
            out = inlines(node.content);
            // A paragraph that looks like the heading convention is protected.
            if ((out.starts_with("[b]") && out.ends_with("[/b]")) || (out.starts_with("[u]") && out.ends_with("[/u]"))) {
              out = "[noparse][/noparse]" + out;
            }
          } else if constexpr (std::same_as<T, ir::Heading>) {
            std::string text = inlines(node.content);
            for (char& c : text) {
              if (c == '\n') c = ' ';
            }
            if (node.level <= 1) out = "[u][b]" + text + "[/b][/u]";
            else if (node.level == 2) out = "[b]" + text + "[/b]";
            else out = "[u]" + text + "[/u]";
          } else if constexpr (std::same_as<T, ir::List>) {
            if (node.definition) {
              for (const ir::Blocks& item : node.items) {
                if (item.empty()) continue;
                if (!out.empty()) out.append("\n");
                if (const auto* p = item.front().get_if<ir::Paragraph>()) out.append(inlines(p->content));
                out.append("\n[list]");
                for (std::size_t k = 1; k < item.size(); ++k) out.append("\n[*]").append(this->block(item[k], depth + 1));
                out.append("\n[/list]");
              }
              return;
            }
            out = node.ordered ? "[list=" + std::to_string(node.start) + "]" : "[list]";
            for (std::size_t i = 0; i < node.items.size(); ++i) {
              out.append("\n[*]");
              if (node.task(i) == ir::TaskState::Done) out.append("\xE2\x98\x91 ");
              if (node.task(i) == ir::TaskState::Open) out.append("\xE2\x98\x90 ");
              out.append(blocks(node.items[i], depth + 1));
            }
            out.append("\n[/list]");
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty() && node.raw_format != "bbcode") return;
            if (node.raw_format == "bbcode") {
              out = node.code;
              return;
            }
            std::string lang = node.language;
            std::erase_if(lang, [](char c) { return !(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '-' || c == '_'); });
            out = (lang.empty() ? "[code]" : "[code=" + lang + "]") + util::replace_all(node.code, "[/code]", "[/ code]") + "\n[/code]";
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            out = "[quote]\n" + blocks(node.blocks, depth + 1) + "\n[/quote]";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            out = "[table]";
            const auto cells = table_strings(node, [this](const ir::Inlines& c) { return inlines(c); });
            for (std::size_t r = 0; r < cells.size(); ++r) {
              out.append("\n[tr]");
              const std::string_view tag = node.rows[r].header ? "th" : "td";
              for (const std::string& cell : cells[r]) out.append("[").append(tag).append("]").append(cell).append("[/").append(tag).append("]");
              out.append("[/tr]");
            }
            out.append("\n[/table]");
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            out = markup_.image(ir::InlineImage{node.source, node.caption.empty() ? node.alt : node.caption});
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "[hr]";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out.clear();
          }
        },
        block.node);
    return out;
  }

  const ir::Document& doc_;
  InlineMarkup markup_;
};

int sniff_bbcode(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"bbcode", "bb"})) return 80;
  const std::string_view head = sniff_head(data);
  int score = 0;
  for (const std::string_view tag : {"[b]", "[i]", "[url=", "[/url]", "[quote]", "[list]", "[img]", "[code]", "[*]"}) {
    if (head.find(tag) != std::string_view::npos) ++score;
  }
  return score >= 4 && !head.starts_with("<") ? 64 : 0;
}

}  // namespace

void register_bbcode(Registry& registry) {
  MarkupFormat bbcode;
  bbcode.id = "bbcode";
  bbcode.name = "BBCode";
  bbcode.description = "Bulletin board markup used by web forums.";
  bbcode.extensions = {"bbcode", "bb"};
  bbcode.mime = "text/x-bbcode";
  bbcode.popularity = 20;
  bbcode.sniff = sniff_bbcode;
  bbcode.read = [](std::string_view text, ReadContext&) -> Result<ir::Document> { return read_bbcode(text); };
  bbcode.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    BbWriter writer(document);
    return writer.run();
  };
  bbcode.write_lossy = true;  // headings are a bold/underline convention, notes become end notes
  register_markup(registry, std::move(bbcode));
}

}  // namespace cvt::document
