// Convertible core - document family: ir::Document -> clean HTML5 / XHTML5.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <span>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/html_internal.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

using html::escape_attribute;
using html::escape_text;

constexpr std::string_view stylesheet =
    "body{font-family:system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;line-height:1.55;max-width:46em;"
    "margin:2em auto;padding:0 1em;color:#1d1d1f;background:#fff}"
    "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.4em 0 .5em}"
    "pre,code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.92em}"
    "pre{background:#f5f5f7;padding:.8em 1em;overflow:auto;border-radius:4px}"
    "code{background:#f5f5f7;padding:.1em .25em;border-radius:3px}pre code{background:none;padding:0}"
    "blockquote{margin:1em 0;padding:0 1em;border-left:4px solid #d2d2d7;color:#424245}"
    "table{border-collapse:collapse;margin:1em 0}th,td{border:1px solid #d2d2d7;padding:.35em .6em;vertical-align:top}"
    "th{background:#f5f5f7}caption{caption-side:top;font-weight:600;padding:.3em}"
    "img{max-width:100%;height:auto}figure{margin:1em 0}figcaption{font-size:.9em;color:#6e6e73}"
    "hr{border:0;border-top:1px solid #d2d2d7;margin:2em 0}"
    ".admonition{border-left-color:#0071e3;background:#f5f9ff;padding:.5em 1em}"
    ".footnotes{font-size:.9em}.task-list-item{list-style:none}.math.display{display:block;text-align:center;margin:1em 0}"
    "@media (prefers-color-scheme:dark){body{background:#1d1d1f;color:#f5f5f7}pre,code,th{background:#2c2c2e}"
    "blockquote{color:#d2d2d7}}";

// Only safe link targets survive: script URLs could run code when the page is opened.
std::string safe_url(std::string_view url) {
  std::string compact;
  for (const char c : url) {
    if (!util::is_ascii_space(c) && static_cast<unsigned char>(c) >= 0x20) compact.push_back(util::ascii_lower(c));
  }
  if (compact.starts_with("javascript:") || compact.starts_with("vbscript:") || compact.starts_with("data:text/html") ||
      compact.starts_with("data:application") || compact.starts_with("data:text/javascript")) {
    return "#";
  }
  return std::string(url);
}

std::string_view align_name(ir::Alignment align) {
  switch (align) {
    case ir::Alignment::Left: return "left";
    case ir::Alignment::Center: return "center";
    case ir::Alignment::Right: return "right";
    case ir::Alignment::Default: return {};
  }
  return {};
}

class Writer {
 public:
  Writer(const ir::Document& document, const HtmlWriteOptions& options) : doc_(document), options_(options) {
    nl_ = options.minify ? "" : options.newline;
  }

  std::string run() {
    std::string body;
    write_blocks(body, doc_.blocks, 0);
    write_notes(body);
    if (!options_.standalone) return body;
    std::string out;
    if (options_.xhtml) out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>").append(nl_);
    out.append("<!DOCTYPE html>").append(nl_);
    out.append("<html");
    if (options_.xhtml) out.append(" xmlns=\"http://www.w3.org/1999/xhtml\"");
    if (!doc_.metadata.language.empty()) {
      out.append(" lang=\"").append(escape_attribute(doc_.metadata.language)).append("\"");
      if (options_.xhtml) out.append(" xml:lang=\"").append(escape_attribute(doc_.metadata.language)).append("\"");
    }
    out.append(">").append(nl_).append("<head>").append(nl_);
    out.append(void_tag("meta charset=\"utf-8\"")).append(nl_);
    out.append(void_tag("meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"")).append(nl_);
    std::string title = document_title(doc_);
    if (title.empty()) title = "Document";
    out.append("<title>").append(escape_text(title)).append("</title>").append(nl_);
    if (options_.metadata) {
      const auto meta = [&](std::string_view name, std::string_view value) {
        if (value.empty()) return;
        out.append(void_tag("meta name=\"" + std::string(name) + "\" content=\"" + escape_attribute(value) + "\""))
            .append(nl_);
      };
      meta("author", doc_.metadata.author);
      meta("date", doc_.metadata.date);
      for (const auto& [key, value] : doc_.metadata.extra) {
        if (key == "description" || key == "keywords" || key == "subject") meta(key, value);
      }
    }
    if (options_.stylesheet) out.append("<style>").append(stylesheet).append("</style>").append(nl_);
    out.append("</head>").append(nl_).append("<body>").append(nl_);
    out.append(body);
    out.append("</body>").append(nl_).append("</html>").append(nl_.empty() ? "" : nl_);
    return out;
  }

 private:
  std::string void_tag(std::string_view content) const {
    return "<" + std::string(content) + (options_.xhtml ? " />" : ">");
  }

  std::string id_attr(const ir::Attributes& attr) const {
    std::string out;
    if (!attr.id.empty()) out.append(" id=\"").append(escape_attribute(attr.id)).append("\"");
    return out;
  }

  void write_blocks(std::string& out, const ir::Blocks& blocks, int depth) {
    for (const ir::Block& block : blocks) write_block(out, block, depth);
  }

  void write_block(std::string& out, const ir::Block& block, int depth) {
    if (depth > ir::max_document_depth * 2) return;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            std::string cls;
            if (block.attr.has_class(ir::conventions::line_block)) cls = " class=\"line-block\"";
            out.append("<p").append(id_attr(block.attr)).append(cls).append(">");
            write_inlines(out, node.content);
            out.append("</p>").append(nl_);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            const std::string level = std::to_string(std::clamp(node.level, 1, 6));
            out.append("<h").append(level).append(id_attr(block.attr)).append(">");
            write_inlines(out, node.content);
            out.append("</h").append(level).append(">").append(nl_);
          } else if constexpr (std::same_as<T, ir::List>) {
            write_list(out, node, block.attr, depth);
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            write_code_block(out, node);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            if (block.attr.has_class(ir::conventions::admonition)) {
              std::string classes;
              for (const std::string& c : block.attr.classes) {
                if (!classes.empty()) classes.push_back(' ');
                classes.append(c);
              }
              out.append("<div class=\"").append(escape_attribute(classes)).append("\"").append(id_attr(block.attr));
              out.append(">").append(nl_);
              write_blocks(out, node.blocks, depth + 1);
              out.append("</div>").append(nl_);
            } else {
              out.append("<blockquote").append(id_attr(block.attr)).append(">").append(nl_);
              write_blocks(out, node.blocks, depth + 1);
              out.append("</blockquote>").append(nl_);
            }
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            write_table(out, node, block.attr);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            write_image_block(out, node, block.attr);
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out.append(void_tag("hr")).append(nl_);
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out.append("<div class=\"page-break\" style=\"break-after:page\"></div>").append(nl_);
          }
        },
        block.node);
  }

  void write_list(std::string& out, const ir::List& list, const ir::Attributes& attr, int depth) {
    if (list.definition) {
      out.append("<dl").append(id_attr(attr)).append(">").append(nl_);
      for (const ir::Blocks& item : list.items) {
        if (item.empty()) continue;
        out.append("<dt>");
        if (const auto* term = item.front().get_if<ir::Paragraph>()) {
          write_inlines(out, term->content);
        } else {
          std::string inner;
          write_block(inner, item.front(), depth + 1);
          out.append(inner);
        }
        out.append("</dt>").append(nl_);
        if (item.size() > 1) {
          out.append("<dd>");
          write_item_blocks(out, std::span<const ir::Block>(item).subspan(1), list.loose, depth);
          out.append("</dd>").append(nl_);
        }
      }
      out.append("</dl>").append(nl_);
      return;
    }
    const std::string_view tag = list.ordered ? "ol" : "ul";
    out.append("<").append(tag).append(id_attr(attr));
    if (attr.has_class("toc")) out.append(" class=\"toc\"");
    if (list.ordered && list.start != 1) out.append(" start=\"").append(std::to_string(list.start)).append("\"");
    if (list.ordered) {
      switch (list.numbering) {
        case ir::ListNumbering::LowerAlpha: out.append(" type=\"a\""); break;
        case ir::ListNumbering::UpperAlpha: out.append(" type=\"A\""); break;
        case ir::ListNumbering::LowerRoman: out.append(" type=\"i\""); break;
        case ir::ListNumbering::UpperRoman: out.append(" type=\"I\""); break;
        case ir::ListNumbering::Decimal: break;
      }
    }
    out.append(">").append(nl_);
    for (std::size_t i = 0; i < list.items.size(); ++i) {
      const ir::TaskState task = list.task(i);
      out.append(task != ir::TaskState::None ? "<li class=\"task-list-item\">" : "<li>");
      if (task != ir::TaskState::None) {
        out.append(void_tag(task == ir::TaskState::Done ? "input type=\"checkbox\" disabled=\"disabled\" checked=\"checked\""
                                                        : "input type=\"checkbox\" disabled=\"disabled\""));
        out.append(" ");
      }
      write_item_blocks(out, list.items[i], list.loose, depth);
      out.append("</li>").append(nl_);
    }
    out.append("</").append(tag).append(">").append(nl_);
  }

  // Tight items write their leading paragraph without <p>.
  void write_item_blocks(std::string& out, std::span<const ir::Block> blocks, bool loose, int depth) {
    bool first = true;
    for (const ir::Block& block : blocks) {
      const auto* p = block.get_if<ir::Paragraph>();
      if (p != nullptr && !loose && block.attr.empty()) {
        if (!first) out.append(void_tag("br"));
        write_inlines(out, p->content);
        first = false;
        continue;
      }
      if (!nl_.empty() && !out.ends_with('\n')) out.append(nl_);
      write_block(out, block, depth + 1);
      first = false;
    }
  }

  void write_code_block(std::string& out, const ir::CodeBlock& code) {
    if (code.math != ir::MathMode::None) {
      out.append("<div class=\"math display\">\\[").append(escape_text(code.code)).append("\\]</div>").append(nl_);
      return;
    }
    if (!code.raw_format.empty()) {
      if (code.raw_format == "html") {
        out.append(code.code).append(nl_);
      }
      return;  // raw content of other formats has no meaning in HTML
    }
    out.append("<pre><code");
    if (!code.language.empty()) out.append(" class=\"language-").append(escape_attribute(code.language)).append("\"");
    out.append(">").append(escape_text(code.code)).append("</code></pre>").append(nl_);
  }

  void write_table(std::string& out, const ir::TableBlock& table, const ir::Attributes& attr) {
    out.append("<table").append(id_attr(attr)).append(">").append(nl_);
    if (!table.caption.empty()) {
      out.append("<caption>");
      write_inlines(out, table.caption);
      out.append("</caption>").append(nl_);
    }
    const std::size_t header_rows = header_row_count(table);
    // Column index tracking for alignment (spans shift columns).
    const TableGrid grid = table_grid(table);
    const auto write_row = [&](std::size_t r) {
      const ir::TableRow& row = table.rows[r];
      out.append("<tr>");
      std::size_t column = 0;
      for (const ir::TableCell& cell : row.cells) {
        while (column < grid.columns && !(grid.cells[r][column] == &cell)) ++column;
        const bool head = row.header;
        out.append(head ? "<th" : "<td");
        if (cell.colspan > 1) out.append(" colspan=\"").append(std::to_string(cell.colspan)).append("\"");
        if (cell.rowspan > 1) out.append(" rowspan=\"").append(std::to_string(cell.rowspan)).append("\"");
        ir::Alignment align = cell.align;
        if (align == ir::Alignment::Default) align = table.column_alignment(column);
        if (align != ir::Alignment::Default) out.append(" style=\"text-align:").append(align_name(align)).append("\"");
        out.append(">");
        write_inlines(out, cell.content);
        out.append(head ? "</th>" : "</td>");
      }
      out.append("</tr>").append(nl_);
    };
    if (header_rows > 0) {
      out.append("<thead>").append(nl_);
      for (std::size_t r = 0; r < header_rows; ++r) write_row(r);
      out.append("</thead>").append(nl_);
    }
    if (header_rows < table.rows.size()) {
      out.append("<tbody>").append(nl_);
      for (std::size_t r = header_rows; r < table.rows.size(); ++r) write_row(r);
      out.append("</tbody>").append(nl_);
    }
    out.append("</table>").append(nl_);
  }

  std::string image_src(const ir::ImageSource& source) const {
    return image_url(source, options_.images == ImageHandling::Embed);
  }

  std::string image_tag(const ir::ImageSource& source, std::string_view alt, std::string_view title) const {
    const std::string src = image_src(source);
    if (src.empty()) return {};
    std::string tag = "img src=\"" + escape_attribute(safe_url(src)) + "\" alt=\"" + escape_attribute(alt) + "\"";
    if (!title.empty()) tag += " title=\"" + escape_attribute(title) + "\"";
    return void_tag(tag);
  }

  void write_image_block(std::string& out, const ir::ImageBlock& image, const ir::Attributes& attr) {
    if (options_.images == ImageHandling::Drop) return;
    const std::string tag = image_tag(image.source, image.alt, image.title);
    if (tag.empty()) {
      const std::string& text = image.caption.empty() ? image.alt : image.caption;
      if (!text.empty()) out.append("<p>").append(escape_text(text)).append("</p>").append(nl_);
      return;
    }
    if (image.caption.empty()) {
      out.append("<p").append(id_attr(attr)).append(">").append(tag).append("</p>").append(nl_);
      return;
    }
    out.append("<figure").append(id_attr(attr)).append(">").append(nl_).append(tag).append(nl_);
    out.append("<figcaption>").append(escape_text(image.caption)).append("</figcaption>").append(nl_);
    out.append("</figure>").append(nl_);
  }

  void write_inlines(std::string& out, const ir::Inlines& inlines, int depth = 0) {
    if (depth > ir::max_document_depth * 2) return;
    for (const ir::Inline& item : inlines) {
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            const auto wrap = [&](std::string_view tag, const ir::Inlines& children) {
              out.append("<").append(tag).append(id_attr(item.attr)).append(">");
              write_inlines(out, children, depth + 1);
              out.append("</").append(tag).append(">");
            };
            if constexpr (std::same_as<T, ir::Text>) {
              out.append(escape_text(node.text));
            } else if constexpr (std::same_as<T, ir::Emphasis>) {
              wrap("em", node.children);
            } else if constexpr (std::same_as<T, ir::Strong>) {
              wrap(item.attr.has_class("mark") ? "mark" : "strong", node.children);
            } else if constexpr (std::same_as<T, ir::Code>) {
              if (node.math != ir::MathMode::None) {
                const bool display = node.math == ir::MathMode::Display;
                out.append(display ? "<span class=\"math display\">\\[" : "<span class=\"math inline\">\\(");
                out.append(escape_text(node.text)).append(display ? "\\]</span>" : "\\)</span>");
              } else if (!node.raw_format.empty()) {
                if (node.raw_format == "html") out.append(node.text);
              } else {
                out.append("<code>").append(escape_text(node.text)).append("</code>");
              }
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                const std::size_t index = footnote_index(doc_, node.note);
                if (index > 0) {
                  const std::string n = std::to_string(index);
                  out.append("<a href=\"#fn-").append(n).append("\" id=\"fnref-").append(n);
                  if (++ref_counts_[index] > 1) out.append("-").append(std::to_string(ref_counts_[index]));
                  out.append("\" class=\"footnote-ref\" role=\"doc-noteref\"><sup>").append(n).append("</sup></a>");
                  return;
                }
              }
              out.append("<a href=\"").append(escape_attribute(safe_url(node.url))).append("\"");
              if (!node.title.empty()) out.append(" title=\"").append(escape_attribute(node.title)).append("\"");
              out.append(id_attr(item.attr)).append(">");
              write_inlines(out, node.children, depth + 1);
              out.append("</a>");
            } else if constexpr (std::same_as<T, ir::Strikethrough>) {
              wrap("del", node.children);
            } else if constexpr (std::same_as<T, ir::Underline>) {
              wrap("u", node.children);
            } else if constexpr (std::same_as<T, ir::Superscript>) {
              wrap("sup", node.children);
            } else if constexpr (std::same_as<T, ir::Subscript>) {
              wrap("sub", node.children);
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              out.append(void_tag("br")).append(options_.minify ? "" : options_.newline);
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              if (options_.images == ImageHandling::Drop) {
                out.append(escape_text(node.alt));
                return;
              }
              const std::string tag = image_tag(node.source, node.alt, node.title);
              out.append(tag.empty() ? escape_text(node.alt) : tag);
            }
          },
          item.node);
    }
  }

  void write_notes(std::string& out) {
    if (doc_.footnotes.empty()) return;
    out.append("<section class=\"footnotes\" role=\"doc-endnotes\">").append(nl_);
    out.append(void_tag("hr")).append(nl_).append("<ol>").append(nl_);
    for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
      const std::string n = std::to_string(i + 1);
      out.append("<li id=\"fn-").append(n).append("\">");
      std::string body;
      const ir::Blocks& blocks = doc_.footnotes[i].blocks;
      const std::string back =
          "<a href=\"#fnref-" + n + "\" class=\"footnote-back\" role=\"doc-backlink\">\xE2\x86\xA9\xEF\xB8\x8E</a>";
      if (blocks.size() == 1 && blocks.front().is<ir::Paragraph>()) {
        write_inlines(body, blocks.front().get_if<ir::Paragraph>()->content);
        body.append(" ").append(back);
      } else {
        body.append(nl_);
        write_blocks(body, blocks, 1);
        body.append("<p>").append(back).append("</p>").append(nl_);
      }
      out.append(body).append("</li>").append(nl_);
    }
    out.append("</ol>").append(nl_).append("</section>").append(nl_);
  }

  const ir::Document& doc_;
  const HtmlWriteOptions& options_;
  std::string nl_;
  std::map<std::size_t, int> ref_counts_;
};

}  // namespace

std::string write_html(const ir::Document& document, const HtmlWriteOptions& options) {
  Writer writer(document, options);
  return writer.run();
}

}  // namespace cvt::document
