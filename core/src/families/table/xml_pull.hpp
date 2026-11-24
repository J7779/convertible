// Convertible core - table family: a minimal streaming XML reader.
//
// Worksheet parts of XLSX and ODS files routinely hold millions of elements.
// Building a DOM for them would cost far more memory than the cells
// themselves, so the readers walk them with this pull parser instead: it
// keeps no tree, decodes entities on demand and holds only the attributes of
// the current start tag. It accepts well-formed UTF-8 XML; DOCTYPE
// declarations are skipped (never interpreted), unknown entities are kept
// literally, and nesting is bounded.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::table {

class XmlPull {
 public:
  enum class Event { StartElement, EndElement, Text, End };

  explicit XmlPull(std::string_view xml, std::size_t max_depth = 512) noexcept;

  /// Advance to the next event. Self-closing elements produce a
  /// StartElement immediately followed by an EndElement. Comments,
  /// processing instructions and the DOCTYPE are skipped. Errors are
  /// Status::Decode (malformed) or Status::Limit (too deep).
  [[nodiscard]] Result<Event> next();

  /// Qualified name of the current start or end element ("x:c").
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  /// Name without its prefix ("c").
  [[nodiscard]] std::string_view local_name() const noexcept;
  /// Decoded value of an attribute of the current start tag, matched by
  /// qualified name first and then by local name; nullopt when absent.
  [[nodiscard]] std::optional<std::string> attribute(std::string_view name) const;
  /// Decoded character data of the current Text event (CDATA included).
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  /// Nesting depth: 1 inside the document element.
  [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
  /// After a StartElement: consume everything up to and including its
  /// matching EndElement.
  [[nodiscard]] Result<void> skip_element();
  /// After a StartElement: the concatenated text of the element and its
  /// descendants; consumes the matching EndElement.
  [[nodiscard]] Result<std::string> element_text();
  /// Byte offset reached, for progress reports.
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

 private:
  [[nodiscard]] Result<Event> read_tag();
  std::string_view xml_;
  std::size_t pos_ = 0;
  std::size_t depth_ = 0;
  std::size_t max_depth_ = 512;
  bool pending_end_ = false;
  std::string_view name_;
  std::string text_;
  std::vector<std::pair<std::string_view, std::string_view>> attributes_;  // raw values
};

/// Decode XML character data (entities and character references).
/// Unknown entities stay as written.
void xml_decode_append(std::string& out, std::string_view raw);

/// The bytes of an XML part as UTF-8 text: UTF-16 (with a byte order mark)
/// is transcoded, a UTF-8 BOM dropped.
[[nodiscard]] std::string xml_text(ByteView data);

/// Escape text for XML content (`attribute` also escapes quotes, tabs and
/// line breaks). Malformed UTF-8 becomes U+FFFD; control characters that
/// XML 1.0 cannot carry are dropped.
void xml_escape_append(std::string& out, std::string_view text, bool attribute = false);

}  // namespace cvt::table
