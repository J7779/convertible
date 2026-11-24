// Convertible core - Document IR: a rich-text tree.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::ir {

/// Readers must not build trees nested deeper than this (lists in quotes in
/// lists..., or emphasis in links in emphasis...). Deeper input is flattened.
/// Recursive consumers may rely on the bound.
inline constexpr int max_document_depth = 64;

// --------------------------------------------------------------------------- images

/// A picture kept in its original encoded form (PNG, JPEG, SVG, ...).
struct EncodedImage {
  Bytes bytes;        ///< the image file
  std::string mime;   ///< e.g. "image/png"; empty when unknown
  /// Where the picture lives when the source only referenced it (an
  /// `<img src="photo.jpg">` or `![](https://...)`): the reference as
  /// written. `bytes` is then EMPTY, because nothing is ever fetched;
  /// writers emit a reference to `url` (or the alt text) instead of data.
  std::string url = {};
};

// --------------------------------------------------------------------------- attributes

/// Optional identifier and annotations of a block or inline, as found in
/// formats that have them (HTML id/class, Markdown `{#id .class}`, Djot,
/// AsciiDoc anchors). Readers fill what they find; writers that cannot
/// express them simply ignore them. A few classes carry agreed meaning
/// (see `ir::conventions`).
struct Attributes {
  std::string id = {};
  std::vector<std::string> classes = {};
  /// Other key/value annotations in source order.
  std::vector<std::pair<std::string, std::string>> pairs = {};

  [[nodiscard]] bool empty() const noexcept { return id.empty() && classes.empty() && pairs.empty(); }
  [[nodiscard]] bool has_class(std::string_view name) const noexcept {
    for (const std::string& c : classes) {
      if (c == name) return true;
    }
    return false;
  }
  void add_class(std::string_view name) {
    if (!has_class(name)) classes.emplace_back(name);
  }
  /// The value of a key/value annotation, or empty.
  [[nodiscard]] std::string_view get(std::string_view key) const noexcept {
    for (const auto& [k, v] : pairs) {
      if (k == key) return v;
    }
    return {};
  }
  void set(std::string_view key, std::string value) {
    for (auto& [k, v] : pairs) {
      if (k == key) {
        v = std::move(value);
        return;
      }
    }
    pairs.emplace_back(std::string(key), std::move(value));
  }
};

/// Class names with an agreed meaning across readers and writers.
namespace conventions {
/// On a BlockQuote: an admonition/callout ("note", "warning", "tip", ... is
/// added as a second class).
inline constexpr std::string_view admonition = "admonition";
/// On a CodeBlock: the output of the code cell before it (notebooks).
inline constexpr std::string_view cell_output = "output";
/// On a Paragraph: a line block (poetry, addresses) whose LineBreaks matter.
inline constexpr std::string_view line_block = "line-block";
}  // namespace conventions

/// How `Code` and `CodeBlock` text is to be understood.
enum class MathMode : std::uint8_t {
  None,     ///< ordinary verbatim text
  Inline,   ///< TeX math in running text (`$x^2$`)
  Display,  ///< TeX math set on its own line (`$$x^2$$`)
};

/// A picture inside a document: either still encoded as found in the source
/// (cheap pass-through between formats that embed files) or decoded pixels.
using ImageSource = std::variant<EncodedImage, Image>;

// --------------------------------------------------------------------------- inlines

struct Inline;
/// A run of inline content.
using Inlines = std::vector<Inline>;

/// Literal characters (UTF-8). Whitespace is significant; a '\n' inside is
/// a soft break that renderers may treat as a space.
struct Text {
  std::string text;
};
/// Emphasised content (usually italic).
struct Emphasis {
  Inlines children;
};
/// Strongly emphasised content (usually bold).
struct Strong {
  Inlines children;
};
/// Inline code / verbatim text.
struct Code {
  std::string text;
  /// Non-None: `text` is TeX math (writers without math show it as code).
  MathMode math = MathMode::None;
  /// Non-empty: `text` is raw markup of that format ("html", "latex", ...)
  /// to be passed through by writers of the same format and shown as code
  /// (or dropped) by others.
  std::string raw_format = {};
};
/// A hyperlink.
struct Link {
  std::string url;    ///< destination, as written in the source
  std::string title;  ///< optional tooltip
  Inlines children;   ///< the link text
  /// Non-empty: this is a footnote reference to `Document::footnotes` entry
  /// with this label; `url` is then "#fn-<label>" and `children` the visible
  /// marker (usually the number), so writers that know nothing of notes
  /// still show a sensible superscript-style link.
  std::string note = {};
};
struct Strikethrough {
  Inlines children;
};
struct Underline {
  Inlines children;
};
struct Superscript {
  Inlines children;
};
struct Subscript {
  Inlines children;
};
/// A forced line break inside a paragraph.
struct LineBreak {};
/// A picture flowing with the text.
struct InlineImage {
  ImageSource source;
  std::string alt;  ///< text alternative
  std::string title = {};  ///< optional tooltip
};

/// One inline node.
struct Inline {
  using Node = std::variant<Text, Emphasis, Strong, Code, Link, Strikethrough, Underline, Superscript, Subscript,
                            LineBreak, InlineImage>;
  Node node;
  /// Optional id/classes; usually empty. Highlighted text (`<mark>`) is a
  /// Strong carrying the class "mark".
  Attributes attr;

  Inline() = default;
  /// Implicit from any node type: `Inlines{Text{"a"}, Strong{{Text{"b"}}}}`.
  template <class T>
    requires(!std::same_as<std::remove_cvref_t<T>, Inline>) && std::constructible_from<Node, T>
  Inline(T&& value) : node(std::forward<T>(value)) {}  // NOLINT(google-explicit-constructor)
  template <class T>
    requires(!std::same_as<std::remove_cvref_t<T>, Inline>) && std::constructible_from<Node, T>
  Inline(T&& value, Attributes attributes) : node(std::forward<T>(value)), attr(std::move(attributes)) {}

  template <class T>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<T>(node);
  }
  template <class T>
  [[nodiscard]] const T* get_if() const noexcept {
    return std::get_if<T>(&node);
  }
  template <class T>
  [[nodiscard]] T* get_if() noexcept {
    return std::get_if<T>(&node);
  }
};

// --------------------------------------------------------------------------- blocks

struct Block;
/// A sequence of block-level content.
using Blocks = std::vector<Block>;

/// A section title.
struct Heading {
  int level = 1;  ///< 1 (top) .. 6
  Inlines content;
};
struct Paragraph {
  Inlines content;
};
/// Check box state of a task-list item.
enum class TaskState : std::uint8_t { None, Open, Done };
/// Numbering style of an ordered list.
enum class ListNumbering : std::uint8_t { Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman };

/// A bulleted or numbered list.
struct List {
  bool ordered = false;       ///< numbered when true
  int start = 1;              ///< number of the first item (ordered lists)
  std::vector<Blocks> items;  ///< each item is a sequence of blocks (often one Paragraph)
  /// A definition list: the FIRST block of every item is the term (a
  /// Paragraph), the remaining blocks are its definition. Writers without
  /// definition lists render it as a bulleted list, which reads fine.
  bool definition = false;
  /// Per item check boxes (GFM task lists); empty, or one entry per item.
  std::vector<TaskState> tasks = {};
  /// Ordered lists only.
  ListNumbering numbering = ListNumbering::Decimal;
  /// Items separated by blank lines (Markdown "loose" lists).
  bool loose = false;

  [[nodiscard]] TaskState task(std::size_t item) const noexcept {
    return item < tasks.size() ? tasks[item] : TaskState::None;
  }
};
/// Preformatted text.
struct CodeBlock {
  std::string language;  ///< syntax hint such as "cpp"; may be empty
  std::string code;      ///< verbatim, lines separated by '\n', no trailing newline required
  /// Display: `code` is TeX math set on its own (writers without math show code).
  MathMode math = MathMode::None;
  /// Non-empty: raw markup of that format ("html", "latex", ...) passed through
  /// by writers of the same format and shown as code by others.
  std::string raw_format = {};
};
struct BlockQuote {
  Blocks blocks;
};
/// One cell of a document table.
enum class Alignment : std::uint8_t { Default, Left, Center, Right };
struct TableCell {
  Inlines content;
  int colspan = 1;
  int rowspan = 1;
  Alignment align = Alignment::Default;  ///< overrides the column alignment when not Default
};
struct TableRow {
  std::vector<TableCell> cells;
  bool header = false;  ///< true for heading rows
};
/// A table embedded in a document. (The spreadsheet IR is `ir::Table`.)
struct TableBlock {
  std::vector<TableRow> rows;
  /// Optional caption shown with the table.
  Inlines caption = {};
  /// Optional per-column alignment; may be shorter than the widest row.
  std::vector<Alignment> alignments = {};

  [[nodiscard]] Alignment column_alignment(std::size_t column) const noexcept {
    return column < alignments.size() ? alignments[column] : Alignment::Default;
  }
};
/// A picture on its own line.
struct ImageBlock {
  ImageSource source;
  std::string alt;      ///< text alternative
  std::string caption;  ///< optional visible caption
  std::string title = {};  ///< optional tooltip
};
/// A thematic break.
struct HorizontalRule {};
/// A forced page break (ignored by formats without pages).
struct PageBreak {};

/// One block node.
struct Block {
  using Node = std::variant<Paragraph, Heading, List, CodeBlock, BlockQuote, TableBlock, ImageBlock, HorizontalRule,
                            PageBreak>;
  Node node;
  /// Optional id/classes (a heading's anchor is `attr.id`); usually empty.
  Attributes attr;

  Block() = default;
  /// Implicit from any node type: `doc.blocks.push_back(Heading{1, {Text{"Hi"}}});`
  template <class T>
    requires(!std::same_as<std::remove_cvref_t<T>, Block>) && std::constructible_from<Node, T>
  Block(T&& value) : node(std::forward<T>(value)) {}  // NOLINT(google-explicit-constructor)
  template <class T>
    requires(!std::same_as<std::remove_cvref_t<T>, Block>) && std::constructible_from<Node, T>
  Block(T&& value, Attributes attributes) : node(std::forward<T>(value)), attr(std::move(attributes)) {}

  template <class T>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<T>(node);
  }
  template <class T>
  [[nodiscard]] const T* get_if() const noexcept {
    return std::get_if<T>(&node);
  }
  template <class T>
  [[nodiscard]] T* get_if() noexcept {
    return std::get_if<T>(&node);
  }
};

// --------------------------------------------------------------------------- document

struct DocumentMetadata {
  std::string title;
  std::string author;
  std::string language;  ///< BCP 47 tag such as "en-GB"; empty when unknown
  std::string date;      ///< ISO 8601 date or date-time; empty when unknown
  /// Anything else worth carrying ("subject", "keywords", "publisher", ...).
  std::map<std::string, std::string> extra;
};

/// The content of a footnote, referenced from the text by a `Link` whose
/// `note` is `label`.
struct Footnote {
  std::string label;  ///< unique within the document (often "1", "2", ...)
  Blocks blocks;
};

/// A text document: metadata plus a tree of blocks.
struct Document {
  DocumentMetadata metadata;
  Blocks blocks;
  /// Footnote bodies, in order of first reference. Writers that cannot hold
  /// notes may append them as end notes (see the document family's
  /// `footnotes_to_endnotes`).
  std::vector<Footnote> footnotes = {};
};

// --------------------------------------------------------------------------- builders

/// Terse constructors for building trees in readers and tests:
///
///   using namespace cvt::ir::build;
///   Document doc;
///   doc.blocks.push_back(heading(1, "Title"));
///   doc.blocks.push_back(paragraph({text("Plain, "), strong("bold"), text(" and "), hyperlink("https://x", "a link")}));
namespace build {

[[nodiscard]] inline Inline text(std::string value) { return Text{std::move(value)}; }
[[nodiscard]] inline Inline code(std::string value) { return Code{std::move(value)}; }
[[nodiscard]] inline Inline line_break() { return LineBreak{}; }
[[nodiscard]] inline Inline emphasis(Inlines children) { return Emphasis{std::move(children)}; }
[[nodiscard]] inline Inline emphasis(std::string value) { return Emphasis{{Text{std::move(value)}}}; }
[[nodiscard]] inline Inline strong(Inlines children) { return Strong{std::move(children)}; }
[[nodiscard]] inline Inline strong(std::string value) { return Strong{{Text{std::move(value)}}}; }
[[nodiscard]] inline Inline strikethrough(Inlines children) { return Strikethrough{std::move(children)}; }
[[nodiscard]] inline Inline underline(Inlines children) { return Underline{std::move(children)}; }
[[nodiscard]] inline Inline superscript(Inlines children) { return Superscript{std::move(children)}; }
[[nodiscard]] inline Inline subscript(Inlines children) { return Subscript{std::move(children)}; }
// Named `hyperlink`, not `link`: POSIX declares ::link(const char*, const char*),
// which would win overload resolution for string literals after a using-directive.
[[nodiscard]] inline Inline hyperlink(std::string url, Inlines children, std::string title = {}) {
  return Link{std::move(url), std::move(title), std::move(children)};
}
[[nodiscard]] inline Inline hyperlink(std::string url, std::string label) {
  return Link{std::move(url), {}, {Text{std::move(label)}}};
}
[[nodiscard]] inline Inline image(ImageSource source, std::string alt = {}) {
  return InlineImage{std::move(source), std::move(alt)};
}

[[nodiscard]] inline Block paragraph(Inlines content) { return Paragraph{std::move(content)}; }
[[nodiscard]] inline Block paragraph(std::string value) { return Paragraph{{Text{std::move(value)}}}; }
[[nodiscard]] inline Block heading(int level, Inlines content) { return Heading{level, std::move(content)}; }
[[nodiscard]] inline Block heading(int level, std::string value) { return Heading{level, {Text{std::move(value)}}}; }
[[nodiscard]] inline Block list(bool ordered, std::vector<Blocks> items, int start = 1) {
  return List{ordered, start, std::move(items)};
}
[[nodiscard]] inline Block code_block(std::string code, std::string language = {}) {
  return CodeBlock{std::move(language), std::move(code)};
}
[[nodiscard]] inline Block block_quote(Blocks blocks) { return BlockQuote{std::move(blocks)}; }
[[nodiscard]] inline Block table(std::vector<TableRow> rows) { return TableBlock{std::move(rows)}; }
[[nodiscard]] inline Block image_block(ImageSource source, std::string alt = {}, std::string caption = {}) {
  return ImageBlock{std::move(source), std::move(alt), std::move(caption)};
}
[[nodiscard]] inline Block horizontal_rule() { return HorizontalRule{}; }
[[nodiscard]] inline Block page_break() { return PageBreak{}; }
/// A table row whose cells each hold one plain string.
[[nodiscard]] inline TableRow row(std::vector<std::string> cells, bool header = false) {
  TableRow out;
  out.header = header;
  out.cells.reserve(cells.size());
  for (std::string& cell : cells) out.cells.push_back(TableCell{{Text{std::move(cell)}}, 1, 1});
  return out;
}

}  // namespace build

// --------------------------------------------------------------------------- plain text

namespace detail {

inline void append_plain(std::string& out, const Inlines& inlines) {
  for (const Inline& item : inlines) {
    std::visit(
        [&out](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, Text> || std::same_as<T, Code>) {
            out.append(node.text);
          } else if constexpr (std::same_as<T, LineBreak>) {
            out.push_back('\n');
          } else if constexpr (std::same_as<T, InlineImage>) {
            out.append(node.alt);
          } else {
            append_plain(out, node.children);
          }
        },
        item.node);
  }
}

inline void append_plain(std::string& out, const Blocks& blocks, std::string_view indent);

inline void append_plain_block(std::string& out, const Block& block, std::string_view indent) {
  const auto line = [&](std::string_view content) {
    // Indent every line of multi-line content.
    std::size_t start = 0;
    while (start <= content.size()) {
      const std::size_t end = content.find('\n', start);
      out.append(indent);
      out.append(content.substr(start, end == std::string_view::npos ? end : end - start));
      out.push_back('\n');
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  };
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, Paragraph> || std::same_as<T, Heading>) {
          std::string content;
          append_plain(content, node.content);
          line(content);
        } else if constexpr (std::same_as<T, List>) {
          int number = node.start;
          for (const Blocks& item : node.items) {
            const std::string marker = node.ordered ? std::to_string(number++) + ". " : std::string("- ");
            std::string body;
            append_plain(body, item, std::string(marker.size(), ' '));
            // Replace the first line's indent with the marker.
            if (body.size() >= marker.size()) {
              body.replace(0, marker.size(), marker);
            } else {
              body = marker + "\n";
            }
            line(std::string_view(body).substr(0, body.size() - 1));
          }
        } else if constexpr (std::same_as<T, CodeBlock>) {
          line(node.code);
        } else if constexpr (std::same_as<T, BlockQuote>) {
          std::string body;
          append_plain(body, node.blocks, "> ");
          if (!body.empty()) line(std::string_view(body).substr(0, body.size() - 1));
        } else if constexpr (std::same_as<T, TableBlock>) {
          for (const TableRow& row : node.rows) {
            std::string content;
            for (std::size_t i = 0; i < row.cells.size(); ++i) {
              if (i > 0) content.push_back('\t');
              append_plain(content, row.cells[i].content);
            }
            line(content);
          }
        } else if constexpr (std::same_as<T, ImageBlock>) {
          if (!node.alt.empty() || !node.caption.empty()) line(node.caption.empty() ? node.alt : node.caption);
        } else if constexpr (std::same_as<T, HorizontalRule>) {
          line("----------------------------------------");
        } else {
          // PageBreak: nothing to show in plain text.
        }
      },
      block.node);
}

inline void append_plain(std::string& out, const Blocks& blocks, std::string_view indent) {
  bool first = true;
  for (const Block& block : blocks) {
    std::string piece;
    append_plain_block(piece, block, indent);
    if (piece.empty()) continue;
    if (!first) {
      // Blank line between blocks, carrying the indent's visible part (e.g. ">").
      std::string_view visible = indent;
      while (!visible.empty() && visible.back() == ' ') visible.remove_suffix(1);
      out.append(visible);
      out.push_back('\n');
    }
    out.append(piece);
    first = false;
  }
}

}  // namespace detail

/// The characters of a run of inlines with all formatting dropped.
[[nodiscard]] inline std::string plain_text(const Inlines& inlines) {
  std::string out;
  detail::append_plain(out, inlines);
  return out;
}

/// A readable plain-text rendition: blocks separated by blank lines, list
/// items marked with "- " or "1. ", quotes prefixed with "> ", table cells
/// separated by tabs, images replaced by their caption or alt text. Ends
/// with a newline unless empty.
[[nodiscard]] inline std::string plain_text(const Blocks& blocks) {
  std::string out;
  detail::append_plain(out, blocks, {});
  return out;
}
[[nodiscard]] inline std::string plain_text(const Document& document) { return plain_text(document.blocks); }

}  // namespace cvt::ir
