// Convertible core - geo family: small helpers for the XML-based formats
// (GPX, KML, TCX, GML, GeoRSS, OSM).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "families/geo/geo.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::geo::xml {

/// Trimmed text of the first child element with this local name ("" when absent).
[[nodiscard]] inline std::string child_text(util::XmlNode node, std::string_view local) {
  const util::XmlNode child = node.child_local(local);
  if (!child) return {};
  return std::string(util::trim(child.text()));
}

/// The child's text as a number, if present and numeric.
[[nodiscard]] inline std::optional<double> child_number(util::XmlNode node, std::string_view local) {
  const util::XmlNode child = node.child_local(local);
  if (!child) return std::nullopt;
  return parse_number(child.text());
}

/// The element's own text as a number.
[[nodiscard]] inline std::optional<double> number(util::XmlNode node) {
  if (!node) return std::nullopt;
  return parse_number(node.text());
}

/// Attribute value as a number.
[[nodiscard]] inline std::optional<double> attribute_number(util::XmlNode node, std::string_view name) {
  const util::XmlAttribute a = node.attribute_local(name);
  if (!a) return std::nullopt;
  return parse_number(a.value());
}

/// Appends `<name>text</name>` (nothing when the text is empty).
inline util::XmlNode add_text(util::XmlNode parent, std::string_view name, std::string_view text) {
  if (text.empty()) return {};
  util::XmlNode child = parent.append_child(name);
  child.append_text(text);
  return child;
}

/// Pretty-printed UTF-8 output with the XML declaration.
[[nodiscard]] inline Bytes finish(const util::XmlDocument& doc) {
  util::XmlWriteOptions options;
  options.indent = "  ";
  return to_bytes(util::xml_to_string(doc, options));
}

/// Parses the input as XML with the geo family's limits, mapping errors to
/// a message that names the format.
[[nodiscard]] inline Result<util::XmlDocument> parse(ByteView input, std::string_view format) {
  auto doc = util::parse_xml(input, xml_options());
  if (!doc) {
    if (doc.error().code == Status::Limit) return fail(doc.error());
    return fail(Status::Decode, "not a valid {} file: {}", format, doc.error().message);
  }
  if (!doc->root()) return fail(Status::Decode, "the {} file has no root element", format);
  return doc;
}

}  // namespace cvt::geo::xml
