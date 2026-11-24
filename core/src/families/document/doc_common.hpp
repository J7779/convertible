// Convertible core - document family: internal helpers shared by the readers
// and writers of this family (not part of the public document_api.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "families/document/document_api.hpp"
#include "ir/document.hpp"
#include "util/bytes.hpp"

namespace cvt::document {

// =========================================================================== registration

/// Which of the shared writer options a format declares.
enum WriterFeature : unsigned {
  feature_wrap = 1u << 0,        ///< `wrap_width`
  feature_standalone = 1u << 1,  ///< `standalone` (complete document vs fragment)
};

/// Describes one text-based document format: the registration helper adds
/// the FormatInfo, a Document reader and/or writer with the shared option
/// pipeline (text decoding, depth limits, metadata stripping, table of
/// contents, heading shift, image handling, line endings).
struct MarkupFormat {
  std::string id;
  std::string name;
  std::string description;
  Category category = Category::Document;
  std::vector<std::string> extensions;
  std::string mime;
  int popularity = 20;
  std::vector<Signature> signatures = {};
  std::function<int(ByteView data, std::string_view filename)> sniff = {};

  /// Reader of decoded UTF-8 text (BOM, UTF-16/32 and legacy encodings are
  /// handled by the pipeline).
  std::function<Result<ir::Document>(std::string_view text, ReadContext& ctx)> read = {};
  /// Reader of raw bytes, for formats that detect their own encoding (HTML,
  /// XML, binary containers). Used instead of `read` when set.
  std::function<Result<ir::Document>(ByteView data, ReadContext& ctx)> read_bytes = {};
  /// Writer; receives the document after the shared transformations. The
  /// pipeline converts '\n' to the chosen line ending afterwards unless
  /// `binary_output` is set.
  std::function<Result<std::string>(const ir::Document& document, WriteContext& ctx)> write = {};

  std::vector<OptionSpec> read_options = {};
  std::vector<OptionSpec> write_options = {};
  unsigned features = 0;          ///< WriterFeature bits
  int default_wrap = 0;           ///< default of `wrap_width`
  bool read_lossy = false;
  bool write_lossy = false;
  int read_cost = 10;
  int write_cost = 10;
  bool binary_output = false;     ///< no line ending conversion
  bool text_options = true;       ///< declare `source_encoding` for `read`
};

void register_markup(Registry& registry, MarkupFormat format);

/// The shared option specs.
[[nodiscard]] OptionSpec option_wrap_width(int default_width);
[[nodiscard]] OptionSpec option_standalone();
[[nodiscard]] std::vector<OptionSpec> common_write_options();

/// Applies strip_metadata / toc / toc_depth / heading_shift / images to a
/// copy of the document, as the pipeline does before every writer.
void apply_write_options(ir::Document& document, const Options& options);
[[nodiscard]] ImageHandling image_handling(const Options& options);
[[nodiscard]] std::string newline_for(const Options& options);

/// Bytes -> UTF-8: BOM, valid UTF-8, BOM-less UTF-16/32, else the label in
/// `fallback` (a charset name understood by decode_charset), warning once.
[[nodiscard]] std::string decode_document_text(ByteView data, const Context& ctx, std::string_view requested = "auto");

// =========================================================================== charsets

/// Decode bytes in a named character set to UTF-8. Knows UTF-8/16/32, ASCII,
/// ISO-8859-1..16, Windows-1250..1258, KOI8-R/U, Mac Roman, IBM 437/850/866.
/// Returns false (and leaves `out` untouched) for an unknown label.
bool decode_charset(ByteView data, std::string_view label, std::string& out);
[[nodiscard]] bool is_known_charset(std::string_view label);

// =========================================================================== inline rendering

/// Markup of one target format for rendering inline nodes. Empty pairs mean
/// "not supported": the children are rendered plainly.
struct InlineMarkup {
  std::function<std::string(std::string_view text)> escape;
  std::array<std::string, 2> strong = {};
  std::array<std::string, 2> emphasis = {};
  std::array<std::string, 2> strikethrough = {};
  std::array<std::string, 2> underline = {};
  std::array<std::string, 2> superscript = {};
  std::array<std::string, 2> subscript = {};
  std::array<std::string, 2> mark = {};  ///< highlighted (Strong with class "mark"); falls back to strong
  std::function<std::string(const ir::Code& code)> code = {};
  /// `label` is the rendered link text.
  std::function<std::string(const ir::Link& link, const std::string& label)> link = {};
  std::function<std::string(const ir::InlineImage& image)> image = {};
  /// Footnote reference (`link.note` set); `index` is 1-based by order of the notes.
  std::function<std::string(const ir::Link& link, std::size_t index)> note = {};
  std::string line_break = "\n";
  /// Called for Text to replace "\n" soft breaks (default: kept).
  std::string soft_break = "\n";
};

/// Render inlines with the given markup. `notes` maps footnote labels to
/// their 1-based index (from the document).
[[nodiscard]] std::string render_inlines(const ir::Inlines& inlines, const InlineMarkup& markup,
                                         const ir::Document* document = nullptr);

/// 1-based index of a footnote label in document.footnotes, 0 when absent.
[[nodiscard]] std::size_t footnote_index(const ir::Document& document, std::string_view label);
[[nodiscard]] const ir::Footnote* find_footnote(const ir::Document& document, std::string_view label);

// =========================================================================== inline parsing

/// One delimiter pair of a lightweight markup language.
enum class InlineKind : std::uint8_t {
  Strong, Emphasis, Strikethrough, Underline, Superscript, Subscript, Code, Mark, Math, Span,
};
struct InlineRule {
  std::string_view open;
  std::string_view close;
  InlineKind kind = InlineKind::Strong;
  bool verbatim = false;  ///< content is not parsed (code, math)
  /// The opener must not be followed by a space and the closer not preceded
  /// by one; the opener must not be preceded / the closer followed by an
  /// alphanumeric character (word boundary).
  bool flanking = true;
  /// Opener/closer may touch letters (intra-word), e.g. Creole "**".
  bool intraword = false;
};

class InlineParser;
/// A hook tried at every position before the delimiter rules: return the
/// number of bytes consumed (having appended to `out`), or 0.
using InlineHook = std::function<std::size_t(InlineParser& parser, std::string_view text, std::size_t position,
                                             ir::Inlines& out)>;

struct InlineSyntax {
  std::vector<InlineRule> rules;
  InlineHook hook = {};
  char escape = 0;              ///< escape character ('\\', '~'), 0 = none
  bool newline_is_break = false;
};

/// Generic left-to-right parser for formats whose inline markup is pairs of
/// delimiters plus a handful of special constructs (links, images).
/// Nesting is limited, runtime is linear in practice.
class InlineParser {
 public:
  explicit InlineParser(const InlineSyntax& syntax) : syntax_(syntax) {}
  [[nodiscard]] ir::Inlines parse(std::string_view text, int depth = 0);
  [[nodiscard]] int depth() const noexcept { return depth_; }

 private:
  const InlineSyntax& syntax_;
  int depth_ = 0;
};

/// Append text to inlines, merging with a trailing Text node.
void append_text(ir::Inlines& out, std::string_view text);

// =========================================================================== blocks and text

/// Lines of a text (LF, CRLF or CR), without terminators.
[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text);
/// Width of leading blanks (tabs to multiples of 4).
[[nodiscard]] std::size_t indentation(std::string_view line);
[[nodiscard]] bool is_blank(std::string_view line);
/// Remove up to `columns` columns of leading blanks.
[[nodiscard]] std::string_view strip_indent(std::string_view line, std::size_t columns);
[[nodiscard]] std::string trim_copy(std::string_view text);
/// The line consists of one character repeated (at least `minimum` times),
/// optionally surrounded by blanks.
[[nodiscard]] bool is_repeated(std::string_view line, char c, std::size_t minimum);

/// Paragraph text joined from lines with '\n'.
[[nodiscard]] std::string join_lines(std::span<const std::string_view> lines);

/// The plain text of inlines on one line (soft breaks and line breaks become spaces).
[[nodiscard]] std::string inline_line(const ir::Inlines& inlines);

/// ASCII grid table: `+---+---+` borders, `|` separators, cells padded to
/// their display width, multi-line cells allowed. `header_separator` is the
/// rule char after the header rows ('=' for reStructuredText, '-' else).
struct GridTableStyle {
  char header_rule = '=';
  bool outer_border = true;
};
[[nodiscard]] std::string render_grid_table(const std::vector<std::vector<std::string>>& cells, std::size_t header_rows,
                                            const std::vector<ir::Alignment>& alignments, const GridTableStyle& style,
                                            std::string_view indent = {});

/// Pad a string to `width` display columns.
[[nodiscard]] std::string pad_to(std::string_view text, std::size_t width, ir::Alignment align = ir::Alignment::Left);

/// Cells of a table rendered to strings by `render`, rectangular, spans flattened.
[[nodiscard]] std::vector<std::vector<std::string>> table_strings(
    const ir::TableBlock& table, const std::function<std::string(const ir::Inlines&)>& render);
/// Number of leading header rows of a table (rows with `header` set).
[[nodiscard]] std::size_t header_row_count(const ir::TableBlock& table);

/// Encode an RGBA frame as PNG (stored with zlib compression).
[[nodiscard]] Bytes encode_png(const ir::Frame& frame);

/// Visit every inline sequence of a document (blocks, table cells, notes, captions).
void for_each_inlines(ir::Document& document, const std::function<void(ir::Inlines&)>& visit);
void for_each_block(ir::Blocks& blocks, const std::function<void(ir::Block&)>& visit, int depth = 0);

/// Metadata title from the first level-1 heading when the metadata has none.
[[nodiscard]] std::string document_title(const ir::Document& document);

/// Lower-case ASCII copy.
[[nodiscard]] std::string lower(std::string_view text);

/// Language of a code block from a class/info string such as
/// "language-python", "lang-py", "sourceCode python" ("python").
[[nodiscard]] std::string language_from_classes(std::string_view classes);

/// Map a whole-document callback over a parsed XML tree helper: text of
/// element and descendants with whitespace collapsed.
[[nodiscard]] std::string collapse_whitespace(std::string_view text);

}  // namespace cvt::document
