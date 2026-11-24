// Convertible core - document family: GNU Texinfo (writer).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <set>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

std::string texi_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '@' || c == '{' || c == '}') out.push_back('@');
    if (c == '\n') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// Inside brace commands with several arguments a comma separates them.
std::string texi_argument(std::string_view text) { return util::replace_all(texi_escape(text), ",", "@comma{}"); }

// Node names may not contain these characters.
std::string node_name(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == ',' || c == ':' || c == '.' || c == '(' || c == ')' || c == '@' || c == '{' || c == '}' || c == '\n') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return collapse_whitespace(out);
}

class TexiWriter {
 public:
  explicit TexiWriter(const ir::Document& document) : doc_(document) {
    m_.escape = texi_escape;
    m_.strong = {"@strong{", "}"};
    m_.emphasis = {"@emph{", "}"};
    m_.underline = {"@emph{", "}"};
    m_.strikethrough = {"@w{", "}"};
    m_.superscript = {"@sup{", "}"};
    m_.subscript = {"@sub{", "}"};
    m_.line_break = "@*\n";
    m_.soft_break = " ";
    m_.code = [](const ir::Code& code) -> std::string {
      if (code.math != ir::MathMode::None) return "@math{" + texi_escape(code.text) + "}";
      if (!code.raw_format.empty()) return code.raw_format == "texi" ? code.text : std::string();
      return "@code{" + texi_escape(code.text) + "}";
    };
    m_.link = [](const ir::Link& link, const std::string& label) -> std::string {
      if (link.url.starts_with("mailto:")) return "@email{" + texi_argument(link.url.substr(7)) + "," + label + "}";
      if (link.url.starts_with("#")) return label;
      if (label == texi_escape(link.url)) return "@url{" + texi_argument(link.url) + "}";
      return "@uref{" + texi_argument(link.url) + "," + util::replace_all(label, ",", "@comma{}") + "}";
    };
    m_.image = [](const ir::InlineImage& image) { return image_command(image.source, image.alt); };
    m_.note = [this](const ir::Link&, std::size_t index) -> std::string {
      if (index == 0 || index > doc_.footnotes.size()) return {};
      std::string body;
      for (const ir::Block& b : doc_.footnotes[index - 1].blocks) {
        if (const auto* p = b.get_if<ir::Paragraph>()) {
          if (!body.empty()) body += " ";
          body += render_inlines(p->content, m_, &doc_);
        }
      }
      return "@footnote{" + body + "}";
    };
  }

  std::string run() {
    std::string title = doc_.metadata.title.empty() ? document_title(doc_) : doc_.metadata.title;
    if (title.empty()) title = "Document";
    std::string out = "\\input texinfo\n@setfilename document.info\n@documentencoding UTF-8\n";
    out += "@settitle " + texi_escape(title) + "\n\n";
    out += "@titlepage\n@title " + texi_escape(title) + "\n";
    if (!doc_.metadata.author.empty()) out += "@author " + texi_escape(doc_.metadata.author) + "\n";
    out += "@end titlepage\n\n";
    // Section structure: Texinfo nests strictly, so levels are made contiguous.
    out += "@node Top\n@top " + texi_escape(title) + "\n\n";
    std::string body;
    int level = 0;
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        const int target = std::clamp(h->level, 1, 4);
        level = std::min(target, level + 1);
        std::string name = node_name(inline_line(h->content));
        if (name.empty()) name = "Section";
        std::string unique = name;
        for (int n = 2; used_nodes_.contains(unique); ++n) unique = name + " " + std::to_string(n);
        used_nodes_.insert(unique);
        static constexpr std::array<std::string_view, 4> commands = {"@chapter", "@section", "@subsection", "@subsubsection"};
        body += "@node " + unique + "\n" + std::string(commands[static_cast<std::size_t>(level - 1)]) + " " +
                render_inlines(h->content, m_, &doc_) + "\n\n";
        continue;
      }
      body += block(b, 0);
    }
    out += body;
    out += "@bye\n";
    return out;
  }

 private:
  static std::string image_command(const ir::ImageSource& source, std::string_view alt) {
    const std::string url = image_url(source, false);
    if (url.empty() || url.find("://") != std::string::npos) return texi_escape(alt);
    std::string base = url;
    const std::size_t dot = base.rfind('.');
    std::string extension;
    if (dot != std::string::npos && dot + 1 < base.size() && base.find('/', dot) == std::string::npos) {
      extension = base.substr(dot);
      base = base.substr(0, dot);
    }
    return "@image{" + texi_argument(base) + ",,," + texi_argument(alt) + "," + texi_argument(extension) + "}";
  }

  std::string paragraph(const ir::Inlines& content) {
    std::string text = render_inlines(content, m_, &doc_);
    // A line starting with '@' that is not a command would be one; text is escaped already.
    return text + "\n\n";
  }

  std::string blocks(const ir::Blocks& list, int depth) {
    std::string out;
    for (const ir::Block& b : list) out += block(b, depth);
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
            // Headings inside lists or quotes: an unnumbered heading.
            out = "@subheading " + render_inlines(node.content, m_, &doc_) + "\n\n";
          } else if constexpr (std::same_as<T, ir::List>) {
            if (node.definition) {
              out = "@table @asis\n";
              for (const ir::Blocks& item : node.items) {
                std::string term;
                if (!item.empty()) {
                  if (const auto* p = item.front().get_if<ir::Paragraph>()) term = render_inlines(p->content, m_, &doc_);
                }
                for (char& c : term) {
                  if (c == '\n') c = ' ';
                }
                ir::Blocks rest(item.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(item.size())), item.end());
                out += "@item " + term + "\n" + blocks(rest, depth + 1);
              }
              out += "@end table\n\n";
              return;
            }
            out = node.ordered ? "@enumerate " + std::to_string(std::max(node.start, 1)) + "\n" : std::string("@itemize @bullet\n");
            for (std::size_t i = 0; i < node.items.size(); ++i) {
              out += "@item\n";
              if (node.task(i) == ir::TaskState::Done) out += "[x] ";
              if (node.task(i) == ir::TaskState::Open) out += "[ ] ";
              out += blocks(node.items[i], depth + 1);
            }
            out += node.ordered ? "@end enumerate\n\n" : "@end itemize\n\n";
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty()) {
              if (node.raw_format == "texi") out = node.code + "\n\n";
              return;
            }
            if (node.math != ir::MathMode::None) {
              out = "@displaymath\n" + texi_escape(node.code) + "\n@end displaymath\n\n";
              return;
            }
            std::string code;
            for (const std::string_view line : util::split(node.code, '\n')) {
              std::string l;
              for (const char c : line) {
                if (c == '@' || c == '{' || c == '}') l.push_back('@');
                l.push_back(c);
              }
              code += l + "\n";
            }
            out = "@example\n" + code + "@end example\n\n";
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            out = "@quotation\n" + blocks(node.blocks, depth + 1) + "@end quotation\n\n";
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            const auto cells = table_strings(node, [this](const ir::Inlines& c) {
              std::string t = render_inlines(c, m_, &doc_);
              for (char& ch : t) {
                if (ch == '\n') ch = ' ';
              }
              return t;
            });
            std::size_t columns = 0;
            for (const auto& row : cells) columns = std::max(columns, row.size());
            if (columns == 0) return;
            out = "@multitable @columnfractions";
            for (std::size_t c = 0; c < columns; ++c) {
              out += " " + util::format_double(1.0 / static_cast<double>(columns)).substr(0, 4);
            }
            out += "\n";
            const std::size_t header = header_row_count(node);
            for (std::size_t r = 0; r < cells.size(); ++r) {
              out += r < header ? "@headitem " : "@item ";
              for (std::size_t c = 0; c < columns; ++c) {
                if (c > 0) out += "\n@tab ";
                out += c < cells[r].size() ? cells[r][c] : std::string();
              }
              out += "\n";
            }
            out += "@end multitable\n\n";
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            out = "@float Figure\n@center " + image_command(node.source, node.alt) + "\n";
            if (!node.caption.empty()) out += "@caption{" + texi_escape(node.caption) + "}\n";
            out += "@end float\n\n";
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            out = "@sp 1\n@noindent\n@center * * *\n\n";
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            out = "@page\n\n";
          }
        },
        block.node);
    return out;
  }

  const ir::Document& doc_;
  InlineMarkup m_;
  std::set<std::string> used_nodes_;
};

int sniff_texinfo(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"texi", "texinfo", "txi"})) return 80;
  const std::string_view head = sniff_head(data, 512);
  return head.starts_with("\\input texinfo") ? 92 : 0;
}

}  // namespace

void register_texinfo(Registry& registry) {
  MarkupFormat texinfo;
  texinfo.id = "texi";
  texinfo.name = "Texinfo";
  texinfo.description = "GNU Texinfo documentation source (written).";
  texinfo.extensions = {"texi", "texinfo", "txi"};
  texinfo.mime = "application/x-texinfo";
  texinfo.popularity = 10;
  texinfo.sniff = sniff_texinfo;
  texinfo.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    TexiWriter writer(document);
    return writer.run();
  };
  texinfo.write_lossy = true;
  register_markup(registry, std::move(texinfo));
}

}  // namespace cvt::document
