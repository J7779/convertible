// Convertible core - document family: a configurable engine for the
// line-oriented wiki markups (MediaWiki, DokuWiki, Creole, Jira, Textile,
// txt2tags, ...). A dialect supplies recognisers for its block syntax and
// its inline delimiters; the engine assembles paragraphs, nested lists,
// tables and quotes, and renders documents back with the dialect's markup.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "families/document/doc_common.hpp"

namespace cvt::document::wiki {

struct Heading {
  int level = 1;
  std::string text;
  std::string id;
};

struct ListItem {
  int depth = 1;           ///< 1 = top level
  bool ordered = false;
  std::string text;        ///< inline markup of the item's first line
  bool term = false;       ///< a definition-list term
  bool definition = false; ///< a definition-list definition
  ir::TaskState task = ir::TaskState::None;
};

struct Cell {
  std::string text;
  bool header = false;
  int colspan = 1;
  int rowspan = 1;
  ir::Alignment align = ir::Alignment::Default;
};

class Reader;

/// Per-parse state shared with the inline hooks of a dialect.
struct Context {
  ir::Document* document = nullptr;
  int note_counter = 0;
  /// Add a footnote with the given body markup; returns the reference node.
  ir::Inline add_note(ir::Blocks body);
};

struct ReadSyntax {
  std::function<std::optional<Heading>(std::string_view line)> heading;
  std::function<std::optional<ListItem>(std::string_view line)> list_item;
  std::function<bool(std::string_view line)> rule;
  std::function<std::optional<std::vector<Cell>>(std::string_view line)> table_row;
  /// Returns the line's content when it belongs to a quote ("> x", "bq. x").
  std::function<std::optional<std::string>(std::string_view line)> quote_line;
  /// Returns the line's text when it is preformatted (leading-space blocks).
  std::function<std::optional<std::string>(std::string_view line)> preformatted_line;
  /// Multi-line constructs (code fences, <pre>, MediaWiki tables, ...):
  /// returns the number of lines consumed (0 = not here) and appends blocks.
  std::function<std::size_t(std::span<const std::string_view> lines, std::size_t at, ir::Blocks& out, Reader& reader)>
      block;
  /// Builds the inline syntax (hooks may capture the context).
  std::function<InlineSyntax(Context& context)> inline_syntax;
  /// Lines of a paragraph are joined with this between them ("\n" soft break).
  bool paragraph_lines_are_breaks = false;
  /// List items continue on following lines that are not blank and not block starts.
  bool list_item_continuation = true;
};

class Reader {
 public:
  Reader(const ReadSyntax& syntax, ir::Document& document);
  /// Parse lines into blocks (recursion for quotes is bounded).
  ir::Blocks parse(std::span<const std::string_view> lines, int depth = 0);
  /// Inline markup of the dialect.
  ir::Inlines inlines(std::string_view text);
  Context& context() { return context_; }

 private:
  const ReadSyntax& syntax_;
  Context context_;
  InlineSyntax inline_syntax_;
};

/// Read a whole text with a dialect: blocks, notes and metadata go into a document.
ir::Document read(std::string_view text, const ReadSyntax& syntax);

// ---------------------------------------------------------------------------------- writing

struct WriteSyntax {
  std::function<std::string(int level, const std::string& text, const ir::Attributes& attr)> heading;
  /// Marker at the start of a list item line (depth 1 = top level; the
  /// numbers of ordered lists are passed for formats that write them).
  std::function<std::string(const std::vector<bool>& ordered_path, int number, ir::TaskState task)> list_marker;
  /// Definition list entry; empty function = render as bulleted list with bold term.
  std::function<std::string(const std::string& term, const std::vector<std::string>& definitions)> definition;
  std::function<std::string(const ir::CodeBlock& code)> code_block;
  /// `inner` is the rendered content of the quote.
  std::function<std::string(const std::string& inner)> quote;
  std::string rule = "----";
  /// Renders a table from rendered cell texts; spans available from `table`.
  std::function<std::string(const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render)>
      table;
  std::function<std::string(const ir::ImageBlock& image, const std::string& url)> image_block;
  /// Footnotes section at the end; empty = notes rendered inline by `markup.note`.
  std::function<std::string(const std::vector<std::string>& bodies)> notes_section;
  /// Text placed between blocks.
  std::string block_separator = "\n\n";
  /// Line break inside list items when an item has several paragraphs.
  std::string item_break = " ";
  /// Escape a line that would otherwise start a block construct.
  std::function<std::string(const std::string& line)> escape_line_start;
  /// Raw markup of this format (e.g. "mediawiki") passes through verbatim.
  std::string raw_format;
};

class Writer {
 public:
  Writer(const ir::Document& document, const WriteSyntax& syntax, InlineMarkup markup, int wrap_width);
  std::string run();
  std::string inlines(const ir::Inlines& content);
  std::string blocks(const ir::Blocks& blocks, int depth);

 private:
  std::string block(const ir::Block& block, int depth);
  std::string list(const ir::List& list, std::vector<bool> path, int depth);
  std::string paragraph(const ir::Inlines& content);

  const ir::Document& doc_;
  const WriteSyntax& syntax_;
  InlineMarkup markup_;
  int wrap_width_ = 0;
};

/// Helpers for dialects.
[[nodiscard]] std::string repeat(std::string_view text, int count);
[[nodiscard]] std::string escape_chars(std::string_view text, std::string_view specials, std::string_view escape = "\\");
/// Every character of `specials` in `text` as a numeric character reference "&#N;".
[[nodiscard]] std::string entity_escape(std::string_view text, std::string_view specials);
/// Length of a bare http(s)/ftp/mailto URL at text[pos] (after a word boundary), or 0.
[[nodiscard]] std::size_t bare_url_length(std::string_view text, std::size_t pos);
/// An HTML character reference at text[pos] ("&amp;", "&#123;"): appends the
/// decoded text and returns its length, or 0.
[[nodiscard]] std::size_t decode_entity(std::string_view text, std::size_t pos, std::string& out);
/// A hook for InlineSyntax that recognises bare URLs and character references.
[[nodiscard]] std::size_t common_inline(std::string_view text, std::size_t pos, ir::Inlines& out);
/// Image source from a reference (data: URIs decoded, others kept as url).
[[nodiscard]] ir::ImageSource image_from_url(std::string_view url);
/// Move per-cell alignments that are uniform down a column into the table's
/// column alignments.
void infer_column_alignment(ir::TableBlock& table);
/// Block bodies made from a single line of inline content.
[[nodiscard]] ir::Blocks paragraph_blocks(ir::Inlines content);

}  // namespace cvt::document::wiki
