// Convertible core - document family: the stable API other families build on.
//
// Office, ebook and PDF code (DOCX, ODT, EPUB, MOBI, PDF, ...) meets HTML,
// Markdown and plain text constantly: EPUB chapters are XHTML, DOCX notes
// become footnotes, PDF text wants wrapping. This header gives them the
// document family's implementations without going through the registry:
//
//   #include "families/document/document_api.hpp"
//
//   auto doc = cvt::document::parse_html(chapter_bytes);          // tolerant HTML5 parser
//   std::string xhtml = cvt::document::write_html(*doc, {.standalone = true, .xhtml = true});
//   std::string md = cvt::document::write_markdown(*doc);
//   std::string text = cvt::document::to_plain_text(*doc, {.wrap_width = 72});
//
// Everything here is pure: no I/O, no globals, deterministic output, never
// throws (out of memory aside). Parsers accept any input: malformed markup
// is repaired, never rejected; only limits (size, depth) produce errors.
// Every returned ir::Document respects ir::max_document_depth.
//
// Dependencies: ir/document.hpp, engine/errors.hpp, util/bytes.hpp only.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "ir/document.hpp"
#include "util/bytes.hpp"

namespace cvt::document {

/// Receives non-fatal notes ("2 images were dropped"); may be null everywhere.
using WarningSink = std::vector<std::string>*;

// =========================================================================== HTML

/// Looks up a resource (picture) referenced by URL, e.g. a member of an EPUB
/// or a part of an MHTML file. Return nullopt to keep the bare reference.
/// Must not do I/O beyond the container it was made for.
using ResourceResolver = std::function<std::optional<ir::EncodedImage>(std::string_view url)>;

struct HtmlReadOptions {
  /// Encoding used when neither a byte order mark nor a <meta charset>
  /// declares one and the bytes are not valid UTF-8 (WHATWG default).
  std::string fallback_encoding = "windows-1252";
  /// Overrides every declaration when not empty ("utf-8", "iso-8859-2", ...).
  std::string forced_encoding = {};
  /// Resolves <img src> that are not data: URIs. Unresolved pictures keep
  /// their URL in ir::EncodedImage::url.
  ResourceResolver resolve = {};
  /// Treat the input as XHTML (case-sensitive, self-closing syntax honoured
  /// everywhere). Detected automatically from an XML declaration when false.
  bool xhtml = false;
  /// Largest number of elements open at once; deeper markup is flattened.
  std::size_t max_open_elements = 512;
  /// Read <title>, <meta name=author|description|keywords|...>, <html lang>.
  bool read_metadata = true;
};

/// Parse HTML or XHTML bytes: BOM / <meta charset> / http-equiv detection
/// and transcoding (UTF-8, UTF-16, UTF-32, Windows-125x, ISO-8859-x, KOI8,
/// Mac Roman, DOS code pages), full HTML5 named character references,
/// implied and misnested tags repaired, script/style/template skipped,
/// whitespace collapsed outside <pre>. Fails only with Status::Limit.
[[nodiscard]] Result<ir::Document> parse_html(ByteView data, const HtmlReadOptions& options = {},
                                              WarningSink warnings = nullptr);
/// The same for text that is already UTF-8 (no encoding detection).
[[nodiscard]] Result<ir::Document> parse_html_text(std::string_view utf8, const HtmlReadOptions& options = {},
                                                   WarningSink warnings = nullptr);

enum class ImageHandling : std::uint8_t {
  Embed,  ///< pictures as data: URIs (HTML) / embedded files (formats that can hold them)
  Link,   ///< keep only references (url, or nothing for embedded-only pictures)
  Drop,   ///< remove pictures; their alt text stays where it fits
};

struct HtmlWriteOptions {
  /// A complete document (doctype, head, title, meta) or just the body content.
  bool standalone = true;
  /// A small embedded stylesheet (standalone only).
  bool stylesheet = true;
  /// XHTML5 (polyglot) syntax: XML declaration, xmlns, self-closed voids.
  bool xhtml = false;
  /// No indentation or optional line breaks.
  bool minify = false;
  ImageHandling images = ImageHandling::Embed;
  /// Write author/date/description/keywords <meta> elements (title and
  /// language are always written when known).
  bool metadata = true;
  /// Line terminator.
  std::string newline = "\n";
};

/// Clean, valid HTML5 (or XHTML5): semantic elements only, every character
/// escaped where needed, pictures as data: URIs, footnotes as an endnote
/// section with back links, never any script.
[[nodiscard]] std::string write_html(const ir::Document& document, const HtmlWriteOptions& options = {});

// =========================================================================== Markdown

enum class MarkdownFlavor : std::uint8_t {
  CommonMark,  ///< the CommonMark specification only
  Gfm,         ///< + tables, strikethrough, task lists, extended autolinks, footnotes
  Extended,    ///< + definition lists, math ($ $$), front matter, heading attributes {#id .class}
};

struct MarkdownReadOptions {
  MarkdownFlavor flavor = MarkdownFlavor::Extended;
  /// Resolves image URLs (as for HTML).
  ResourceResolver resolve = {};
  /// Largest nesting of block containers (quotes, lists); deeper is flattened.
  int max_nesting = 32;
  /// Convert HTML blocks (figures, tables, comments) into document structure
  /// with the HTML reader; off keeps them as raw "html" CodeBlocks.
  bool parse_html_blocks = true;
};

/// CommonMark-compliant parsing plus the flavour's extensions. Raw HTML is
/// kept as raw ("html") Code / CodeBlock nodes. YAML (---) or TOML (+++)
/// front matter fills metadata (title, author, date, lang) and is kept
/// verbatim in metadata.extra["front_matter"] (+ "front_matter_format").
[[nodiscard]] Result<ir::Document> parse_markdown(std::string_view text, const MarkdownReadOptions& options = {},
                                                  WarningSink warnings = nullptr);

struct MarkdownWriteOptions {
  MarkdownFlavor flavor = MarkdownFlavor::Gfm;
  /// Wrap paragraphs at this column; 0 = one line per paragraph.
  int wrap_width = 0;
  /// [text][1] with definitions at the end instead of [text](url).
  bool reference_links = false;
  /// Underlined (===/---) level 1-2 headings instead of # prefixes.
  bool setext_headings = false;
  /// Write metadata as YAML front matter (Extended flavour, or any flavour
  /// when the source had front matter).
  bool front_matter = true;
  ImageHandling images = ImageHandling::Embed;
  char bullet = '-';       ///< '-', '*' or '+'
  char emphasis = '*';     ///< '*' or '_'
  std::string newline = "\n";
};

/// Markdown that parses back (with parse_markdown and the same flavour) to
/// the same document: text is escaped only where it would otherwise be
/// markup. Constructs the flavour lacks degrade to raw HTML (tables with
/// spans, underline, sub/superscript) or plain text.
[[nodiscard]] std::string write_markdown(const ir::Document& document, const MarkdownWriteOptions& options = {});

// =========================================================================== plain text

struct PlainTextOptions {
  /// Wrap paragraphs at this column (display width: wide CJK counts 2); 0 = no wrapping.
  int wrap_width = 72;
  /// Underline level 1 headings with '=' and level 2 with '-'; deeper
  /// levels get no underline. Off: headings are plain lines.
  bool underline_headings = true;
  /// Draw tables with +---+ borders; off: cells separated by tabs.
  bool ascii_tables = true;
  /// Link targets as numbered references ("text [1]" ... "[1] https://...");
  /// off: "text <https://...>" inline. Links whose text is the URL stay as is.
  bool link_references = true;
  /// Bullet for unordered lists.
  std::string bullet = "*";
  /// Footnotes are always appended as "[1] ..." after a separator line.
  std::string newline = "\n";
};

/// A well laid-out plain-text rendition (the Document -> PlainText bridge).
[[nodiscard]] std::string to_plain_text(const ir::Document& document, const PlainTextOptions& options = {});

struct TextInferenceOptions {
  bool headings = true;      ///< underlined lines, "1. Title" numbered sections, ALL-CAPS short lines
  bool lists = true;         ///< "-", "*", "+", "•", "1.", "a)" items
  bool code = true;          ///< blocks indented by 4+ spaces or a tab
  bool quotes = true;        ///< "> " prefixed blocks
  bool rules = true;         ///< lines of ---, ***, ===, ___
};
/// Structure guessed from plain text (the PlainText -> Document bridge with
/// `infer_structure` on). Soft line breaks inside paragraphs are joined.
[[nodiscard]] ir::Document infer_document_from_text(std::string_view text, const TextInferenceOptions& options = {});

// =========================================================================== utilities

/// GitHub-style anchor: lower case, letters/digits/'-'/'_' kept (any script),
/// spaces to '-', everything else dropped. "Hello, World!" -> "hello-world".
[[nodiscard]] std::string slugify(std::string_view text);
/// slugify() made unique against `used` ("intro", "intro-1", ...); inserts it.
[[nodiscard]] std::string unique_slug(std::string_view text, std::set<std::string>& used);

struct OutlineEntry {
  int level = 1;
  std::string text;  ///< plain text of the heading
  std::string id;    ///< the heading's anchor (Block::attr.id), may be empty
};
/// Every heading of the document in reading order (also inside quotes and lists).
[[nodiscard]] std::vector<OutlineEntry> heading_outline(const ir::Document& document);
/// Give every heading without an id a unique slug id.
void assign_heading_ids(ir::Document& document);
/// A nested list of links to the headings up to `max_level` (assigns ids first).
[[nodiscard]] ir::Block table_of_contents(ir::Document& document, int max_level = 3);
/// Insert table_of_contents() after a leading title heading (or at the top),
/// preceded by a "Contents" heading. No-op for documents without headings.
void insert_table_of_contents(ir::Document& document, int max_level = 3);

/// Relabel footnotes "1", "2", ... in order of first reference, update the
/// reference markers, drop notes that are never referenced and add empty
/// notes for dangling references.
void number_footnotes(ir::Document& document);
/// For writers without notes: append the notes as a numbered "Notes" list at
/// the end (with anchors "fn-N") and leave the references as links to them.
void footnotes_to_endnotes(ir::Document& document);

/// Add `delta` to every heading level, clamped to 1..6.
void shift_headings(ir::Document& document, int delta);
/// Privacy: drop author, date and every extra metadata entry (keeps the title
/// and language, which are content).
void strip_metadata(ir::Document& document);
/// Replace pictures by references or remove them (see ImageHandling).
void apply_image_handling(ir::Document& document, ImageHandling handling);

/// A table resolved into a rectangular grid: `cells[r][c]` points at the
/// cell covering that slot; `origin[r][c]` is true where that cell starts
/// (false in slots covered by a row/col span). Spans running past the table
/// are clipped.
struct TableGrid {
  std::vector<std::vector<const ir::TableCell*>> cells;
  std::vector<std::vector<bool>> origin;
  std::size_t columns = 0;
  std::size_t header_rows = 0;  ///< leading rows marked `header`
};
[[nodiscard]] TableGrid table_grid(const ir::TableBlock& table);
/// A rectangular copy without spans: a spanned cell keeps its content in its
/// first slot and the covered slots become empty cells (or copies with
/// `repeat_spanned`). Rows keep their order and their `header` flag.
[[nodiscard]] ir::TableBlock normalize_table(const ir::TableBlock& table, bool repeat_spanned = false);

/// Merge adjacent Text nodes and drop empty Text and empty formatting nodes.
void normalize_inlines(ir::Inlines& inlines);
/// The same for every inline sequence in the document.
void normalize_document(ir::Document& document);
/// Flatten anything nested deeper than `max_depth` (blocks and inlines).
void enforce_depth(ir::Document& document, int max_depth = ir::max_document_depth);

/// Columns a string occupies in a monospaced terminal (East Asian wide and
/// emoji count 2, combining marks 0).
[[nodiscard]] std::size_t display_width(std::string_view utf8);
/// Greedy word wrap at `width` columns; words longer than a line stay whole.
/// `first_prefix` starts the first line, `prefix` every following one.
/// Existing '\n' are kept as forced breaks. Lines never end in spaces.
[[nodiscard]] std::string wrap_text(std::string_view text, std::size_t width, std::string_view first_prefix = {},
                                    std::string_view prefix = {}, std::string_view newline = "\n");

/// Media type from the leading bytes (PNG, JPEG, GIF, WebP, BMP, TIFF, SVG,
/// ICO, AVIF/HEIC, JXL); empty when unknown.
[[nodiscard]] std::string sniff_image_mime(ByteView data);
/// The picture as an encoded file: encoded images are returned as they are
/// (mime sniffed when missing); decoded pixels are encoded as PNG. nullopt
/// for empty / external-only pictures.
[[nodiscard]] std::optional<ir::EncodedImage> encoded_image(const ir::ImageSource& source);
/// "data:image/png;base64,...", or empty when the picture has no bytes.
[[nodiscard]] std::string image_data_uri(const ir::ImageSource& source);
/// The URL to reference a picture by: its external url, else a data: URI
/// when `allow_data_uri`, else empty.
[[nodiscard]] std::string image_url(const ir::ImageSource& source, bool allow_data_uri);
/// Decode "data:[mime][;base64],payload" into an encoded picture (nullopt
/// when malformed or not a data: URI).
[[nodiscard]] std::optional<ir::EncodedImage> parse_data_uri_image(std::string_view uri);

}  // namespace cvt::document
