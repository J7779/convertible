// Convertible core - document family: Markdown internals shared by the block
// parser, the inline parser and the writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "families/document/document_api.hpp"
#include "ir/document.hpp"

namespace cvt::document::markdown {

struct LinkReference {
  std::string url;
  std::string title;
};

/// Shared state of one parse.
struct ParseState {
  MarkdownFlavor flavor = MarkdownFlavor::Extended;
  const MarkdownReadOptions* options = nullptr;
  /// Normalised label -> definition (first definition wins).
  std::map<std::string, LinkReference> references;
  /// Normalised footnote labels that have a definition, -> original label.
  std::map<std::string, std::string> footnote_labels;
  /// Footnote labels referenced from the text (normalised), in order.
  std::set<std::string> footnotes_used;

  [[nodiscard]] bool gfm() const noexcept { return flavor != MarkdownFlavor::CommonMark; }
  [[nodiscard]] bool extended() const noexcept { return flavor == MarkdownFlavor::Extended; }
};

/// CommonMark link label normalisation: case fold, collapse whitespace, trim.
[[nodiscard]] std::string normalize_label(std::string_view label);

/// Parse inline content (a paragraph's text, a heading, a table cell).
[[nodiscard]] ir::Inlines parse_inlines(std::string_view text, ParseState& state);

/// Try to read a link reference definition at the start of `text`; on
/// success records it in `state` and returns the bytes consumed, else 0.
[[nodiscard]] std::size_t parse_reference_definition(std::string_view text, ParseState& state);

/// Character reference ("&amp;", "&#123;", "&#x1F600;") at text[pos]; appends
/// the value and returns its length, or 0.
[[nodiscard]] std::size_t parse_entity(std::string_view text, std::size_t pos, std::string& out);

/// Unescape backslash escapes and entities (link destinations, titles, info strings).
[[nodiscard]] std::string unescape_string(std::string_view text);

[[nodiscard]] constexpr bool is_ascii_punctuation(char c) noexcept {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

/// Raw HTML recognisers at text[pos]: length of the construct or 0.
[[nodiscard]] std::size_t match_open_tag(std::string_view text, std::size_t pos);
[[nodiscard]] std::size_t match_close_tag(std::string_view text, std::size_t pos);
/// Any inline raw HTML: tag, comment, processing instruction, declaration, CDATA.
[[nodiscard]] std::size_t match_raw_html(std::string_view text, std::size_t pos);

/// Image from a destination: data: URIs decoded, others resolved or kept as url.
[[nodiscard]] ir::ImageSource image_source(std::string_view url, const ParseState& state);

}  // namespace cvt::document::markdown
