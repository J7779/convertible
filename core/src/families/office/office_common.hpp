// Convertible core - office family: shared building blocks.
//
// Everything the office readers and writers have in common: a tolerant
// package reader on top of util::ZipReader (OPC relationships, content
// types), XML parsing with hostile-input settings, a run-based inline
// builder that turns "runs with formatting flags" (DOCX w:r, RTF groups,
// ODF spans, DOC CHPX runs) into a clean ir::Inlines tree, a list builder
// that nests flat "paragraph at list level n" sequences, picture helpers
// (size sniffing, the optimise/drop image policy of the writers) and the
// options shared by every reader and writer of the family.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/document.hpp"
#include "util/bytes.hpp"
#include "util/xml.hpp"
#include "util/zip.hpp"

namespace cvt::office {

// =========================================================================== XML

/// Parse XML coming from an untrusted file: entity expansion bounded,
/// nesting bounded, invalid characters replaced instead of failing.
[[nodiscard]] Result<util::XmlDocument> parse_untrusted_xml(ByteView data, bool preserve_whitespace = true);

/// Attribute by local name (namespace prefix ignored).
[[nodiscard]] std::string_view attr(util::XmlNode node, std::string_view local_name);
/// First child element with this local name.
[[nodiscard]] util::XmlNode child(util::XmlNode node, std::string_view local_name);
/// All child elements with this local name.
[[nodiscard]] std::vector<util::XmlNode> children(util::XmlNode node, std::string_view local_name);
/// First descendant element with this local name (depth-first, bounded).
[[nodiscard]] util::XmlNode descendant(util::XmlNode node, std::string_view local_name);
/// Escape for XML text / attributes (thin wrapper over util::xml_escape).
[[nodiscard]] std::string esc(std::string_view text);
[[nodiscard]] std::string esc_attr(std::string_view text);

// =========================================================================== packages

/// A ZIP package (OPC, ODF, EPUB ...) with case-insensitive part lookup and
/// relationship parsing.
class Package {
 public:
  [[nodiscard]] static Result<Package> open(ByteView data, const Limits& limits);
  /// A package whose parts are already in memory (flat OPC, tests).
  [[nodiscard]] static Package from_parts(std::map<std::string, Bytes> parts);

  [[nodiscard]] bool has(std::string_view name) const;
  /// Part bytes; Status::Decode when missing.
  [[nodiscard]] Result<Bytes> read(std::string_view name);
  /// Part bytes or nullopt when missing / unreadable.
  [[nodiscard]] std::optional<Bytes> try_read(std::string_view name);
  /// Parsed XML part.
  [[nodiscard]] Result<util::XmlDocument> xml(std::string_view name, bool preserve_whitespace = true);
  /// Every part name, in stored order.
  [[nodiscard]] std::vector<std::string> names() const;
  [[nodiscard]] bool encrypted_entries() const;

  struct Relationship {
    std::string id;
    std::string type;    ///< full type URI
    std::string target;  ///< resolved part name (no leading '/') or the external URL
    bool external = false;
  };
  /// Relationships of a part ("" for the package relationships).
  [[nodiscard]] std::vector<Relationship> relationships(std::string_view part);
  /// Content type of a part from [Content_Types].xml (overrides, then defaults).
  [[nodiscard]] std::string content_type(std::string_view part);

 private:
  Package() = default;
  [[nodiscard]] std::optional<std::size_t> index_of(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> part_key(std::string_view name) const;
  std::optional<util::ZipReader> zip_;
  std::map<std::string, Bytes> parts_;
  bool types_loaded_ = false;
  std::map<std::string, std::string> type_defaults_;
  std::map<std::string, std::string> type_overrides_;
};

/// "word/_rels/document.xml.rels" for "word/document.xml" ("_rels/.rels" for "").
[[nodiscard]] std::string rels_path_for(std::string_view part);
/// Resolve a relative target against the folder of `source_part` ("../media/a.png").
[[nodiscard]] std::string resolve_part(std::string_view source_part, std::string_view target);
/// Percent-decode a URI path component ("a%20b" -> "a b").
[[nodiscard]] std::string percent_decode(std::string_view text);

/// Tell-tale member names of a ZIP package without opening it fully (works
/// on truncated files): true when the local headers name `member`.
[[nodiscard]] bool zip_mentions(ByteView data, std::string_view member, std::size_t window = 64 * 1024);
/// The content type of the main part of an OOXML package (the Override for
/// `main_part`, e.g. "/word/document.xml"), read from [Content_Types].xml
/// under small limits; empty when unavailable.
[[nodiscard]] std::string ooxml_main_content_type(ByteView data, std::string_view main_part);
/// The media type stored in an ODF/EPUB style leading "mimetype" member.
[[nodiscard]] std::string leading_mimetype(ByteView data);

// =========================================================================== inlines

/// Character formatting of one run.
struct RunFormat {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  bool superscript = false;
  bool subscript = false;
  bool code = false;  ///< monospaced font
  bool hidden = false;
  std::string link;   ///< hyperlink target ("" = none)
  [[nodiscard]] bool operator==(const RunFormat&) const = default;
};

/// Collects runs and produces a minimal inline tree: adjacent runs with the
/// same formatting are merged, formatting nests Link > Strong > Emphasis >
/// Underline > Strikethrough > Superscript/Subscript > Code.
class InlineBuilder {
 public:
  void text(std::string_view utf8, const RunFormat& format);
  /// A non-text inline (image, line break, footnote reference ...).
  void node(ir::Inline inline_node, const RunFormat& format);
  void line_break(const RunFormat& format) { node(ir::LineBreak{}, format); }
  [[nodiscard]] bool empty() const noexcept { return pieces_.empty(); }
  /// True when only whitespace text has been added.
  [[nodiscard]] bool blank() const;
  [[nodiscard]] ir::Inlines take();
  /// Plain text added so far.
  [[nodiscard]] std::string plain() const;

 private:
  struct Piece {
    RunFormat format;
    std::optional<ir::Inline> node;  ///< non-text node, else text
    std::string text;
  };
  std::vector<Piece> pieces_;
};

/// Trim leading/trailing whitespace of a paragraph's inline content (Text
/// nodes at the edges, recursively) and drop empties.
void trim_inlines(ir::Inlines& inlines);

// =========================================================================== lists

/// Builds nested lists from flat "paragraph at level n of list L" input.
class FlowBuilder {
 public:
  struct ListInfo {
    std::string list_id;  ///< items with different ids never share a list
    int level = 0;        ///< 0-based nesting level
    bool ordered = false;
    ir::ListNumbering numbering = ir::ListNumbering::Decimal;
    int start = 1;
  };
  /// An ordinary block: closes any open list.
  void block(ir::Block block);
  /// One list item paragraph (or other block) at a level.
  void list_item(const ListInfo& info, ir::Block block);
  /// Append a block to the current (deepest) list item, if a list is open;
  /// otherwise like block(). Used for continuation paragraphs.
  void continuation(ir::Block block);
  [[nodiscard]] bool in_list() const noexcept { return !stack_.empty(); }
  void close_lists();
  [[nodiscard]] ir::Blocks take();

 private:
  struct Open {
    ir::List list;
    ListInfo info;
  };
  void close_to(std::size_t depth);
  ir::Blocks out_;
  std::vector<Open> stack_;
};

// =========================================================================== pictures

struct PixelSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};
/// Dimensions from the header of a PNG, JPEG, GIF, BMP, TIFF or WebP file.
[[nodiscard]] std::optional<PixelSize> sniff_image_size(ByteView data);
/// File extension for a picture media type ("png", "jpeg", "gif", ...).
[[nodiscard]] std::string extension_for_mime(std::string_view mime);
/// Media type from a file name extension.
[[nodiscard]] std::string mime_for_extension(std::string_view name);

/// What a writer does with pictures.
enum class ImageMode : std::uint8_t { Keep, Optimize, Drop };

struct ImagePolicy {
  ImageMode mode = ImageMode::Keep;
  int quality = 90;            ///< JPEG quality when re-encoding
  std::uint32_t max_size = 0;  ///< longest side in pixels; 0 = unlimited
};
/// The writer options `images`, `quality`, `preset`, `max_image_size`.
[[nodiscard]] ImagePolicy image_policy(const Options& options);

/// A picture ready to embed: encoded bytes, media type and pixel size.
struct PreparedImage {
  Bytes bytes;
  std::string mime;
  std::string extension;
  PixelSize size;
};
/// Encode (decoded pixels -> PNG), optionally downscale/re-encode (Optimize:
/// the re-encoded file is used only when it is smaller), and measure. nullopt
/// for pictures without data (external references) or with Drop.
[[nodiscard]] std::optional<PreparedImage> prepare_image(const ir::ImageSource& source, const ImagePolicy& policy,
                                                         const Context& ctx);

/// True for the picture formats every office suite displays (png, jpg, gif, bmp, tiff).
[[nodiscard]] bool is_web_safe(std::string_view extension);
/// Re-encode a picture as PNG through the registry; nullopt when no decoder exists.
[[nodiscard]] std::optional<PreparedImage> convert_to_png(const PreparedImage& image, const Context& ctx);

/// Deduplicating store of the pictures of one output package.
class MediaStore {
 public:
  struct Item {
    std::string name;  ///< "image3.png"
    PreparedImage image;
  };
  /// Index of the stored picture (same bytes -> same item).
  std::size_t add(PreparedImage image, std::string_view prefix = "image");
  [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }

 private:
  std::vector<Item> items_;
};

// =========================================================================== options

/// Reader options shared by the word-processing readers.
[[nodiscard]] std::vector<OptionSpec> document_read_options();
/// Writer options shared by the paginated writers (DOCX, ODT, RTF ...).
[[nodiscard]] std::vector<OptionSpec> document_write_options(bool pages = true);
/// The `images` / `quality` / `max_image_size` writer specs.
[[nodiscard]] std::vector<OptionSpec> image_write_options();

/// Page geometry in twentieths of a point (twips) from `page_size`,
/// `orientation` and `margin_mm`.
struct PageSetup {
  int width = 11906;   ///< A4
  int height = 16838;
  int margin = 1440;   ///< 25.4 mm
};
[[nodiscard]] PageSetup page_setup(const Options& options);

struct ReadSettings {
  bool comments = false;
  bool headers = false;
  bool notes = true;          ///< footnotes/endnotes (and speaker notes)
  bool hidden = false;
  bool tracked_changes = false;  ///< show deletions (struck) and insertions (underlined)
  bool strict_headings = false;  ///< only outline levels / heading styles, no guessing
  bool metadata = false;         ///< keep identifying metadata (inverse of strip_metadata)
};
[[nodiscard]] ReadSettings read_settings(const Options& options);

/// Fill a heading level from a style name ("Heading 2", "heading2",
/// "Title", "Überschrift 1" ...); 0 when it is not a heading style.
[[nodiscard]] int heading_level_from_style_name(std::string_view name);

/// Apply the privacy rules to a document read from a file.
void finish_metadata(ir::Document& document, const ReadSettings& settings);

/// Header rows are emphasised by their role already: a header cell whose
/// whole content is bold loses that redundant Strong.
void unwrap_header_emphasis(ir::TableBlock& table);

/// Merge consecutive code blocks and block quotes (readers that see one
/// paragraph at a time produce one block per line).
void merge_adjacent_blocks(ir::Blocks& blocks);

/// Split text at '\n' into Text and LineBreak inlines.
[[nodiscard]] ir::Inlines text_with_breaks(std::string_view text);

/// Format a decimal with up to 3 fraction digits, locale-free.
[[nodiscard]] std::string fmt_number(double value);

}  // namespace cvt::office
