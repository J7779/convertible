// Convertible core - document family: the in-house HTML5 tokenizer and tree
// builder (internal; other families use document_api.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::document::html {

/// Value of a named character reference (`name` without '&', with the ';'
/// when the reference has one, e.g. "amp;" or the legacy "amp"); empty
/// when unknown.
[[nodiscard]] std::string_view lookup_named_reference(std::string_view name) noexcept;

/// Decode character references in text. `in_attribute` applies the legacy
/// rule that "&name" without ';' followed by '=' or an alphanumeric stays
/// literal.
[[nodiscard]] std::string decode_references(std::string_view text, bool in_attribute);

enum class NodeType : std::uint8_t { Document, Element, Text, Comment };

using NodeId = std::uint32_t;
inline constexpr NodeId no_node = 0xFFFFFFFFu;

struct Node {
  NodeType type = NodeType::Element;
  std::string name;  ///< lower-case tag name (elements)
  std::vector<std::pair<std::string, std::string>> attributes;  ///< lower-case names, decoded values
  std::string text;  ///< Text / Comment content (decoded)
  NodeId parent = no_node;
  std::vector<NodeId> children;
  bool foreign = false;  ///< inside <svg> or <math>
};

/// A parsed page: node 0 is the document; the tree never has cycles and is
/// at most `max_open_elements` + 1 deep.
struct Dom {
  std::vector<Node> nodes;
  std::string encoding;  ///< the character set the bytes were decoded from
  /// Declarations per class name from the page's <style> elements
  /// ("s4" -> "text-decoration: line-through;"), for presentational markup
  /// written by word processors (`span.s4 {...}`). Simple selectors only.
  std::map<std::string, std::string, std::less<>> class_rules;
  std::vector<std::string> warnings;

  [[nodiscard]] const Node& node(NodeId id) const { return nodes[id]; }
  [[nodiscard]] std::string_view attribute(NodeId id, std::string_view name) const;
  [[nodiscard]] bool has_attribute(NodeId id, std::string_view name) const;
  /// Text content of a subtree (all Text descendants concatenated).
  [[nodiscard]] std::string text_content(NodeId id) const;
  /// First element with this name in document order, or no_node.
  [[nodiscard]] NodeId find_first(std::string_view name, NodeId from = 0) const;
};

struct ParseSettings {
  bool xhtml = false;
  std::size_t max_open_elements = 512;
  std::size_t max_nodes = 20'000'000;
};

/// Tokenize and build the tree from UTF-8 text.
[[nodiscard]] Dom parse_dom(std::string_view utf8, const ParseSettings& settings);

/// Charset of HTML bytes: BOM, else a <meta charset> / http-equiv in the
/// first 1024 bytes, else empty.
[[nodiscard]] std::string prescan_charset(ByteView data);

/// Serialise a subtree back to markup (used for inline SVG).
[[nodiscard]] std::string serialize(const Dom& dom, NodeId id);

/// HTML void elements (<br>, <img>, ...).
[[nodiscard]] bool is_void_element(std::string_view name) noexcept;

/// Escape text / attribute values for HTML output.
[[nodiscard]] std::string escape_text(std::string_view text);
[[nodiscard]] std::string escape_attribute(std::string_view text);

}  // namespace cvt::document::html
