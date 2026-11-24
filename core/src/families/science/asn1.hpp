// Convertible core - science family: a strict ASN.1 DER decoder and the small
// DER encoder used to re-encode public keys and certificate bags.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "families/science/common.hpp"

namespace cvt::science::asn1 {

struct Node {
  std::uint8_t cls = 0;        ///< 0 universal, 1 application, 2 context, 3 private
  bool constructed = false;
  std::uint32_t tag = 0;
  ByteView header_and_value;   ///< the whole TLV
  ByteView value;
  std::vector<Node> children;  ///< for constructed nodes (and parsed OCTET/BIT STRING wrappers when `parse_wrapped`)

  [[nodiscard]] bool is(std::uint8_t c, std::uint32_t t) const { return cls == c && tag == t; }
  [[nodiscard]] bool universal(std::uint32_t t) const { return cls == 0 && tag == t; }
  [[nodiscard]] const Node* child(std::size_t i) const { return i < children.size() ? &children[i] : nullptr; }
};

/// Parse exactly one DER element covering all of `data` (no trailing bytes).
[[nodiscard]] Result<Node> parse(ByteView data, int max_depth = max_nesting);
/// Parse a sequence of elements (e.g. the content of a SET OF).
[[nodiscard]] Result<std::vector<Node>> parse_all(ByteView data, int max_depth = max_nesting);

[[nodiscard]] std::string oid_text(ByteView value);
[[nodiscard]] std::string oid_name(std::string_view dotted);  ///< "commonName" or the dotted form
[[nodiscard]] std::string integer_text(ByteView value);       ///< decimal for small, hex otherwise
[[nodiscard]] std::string time_text(const Node& node);        ///< ISO 8601 from UTCTime/GeneralizedTime
[[nodiscard]] std::string string_text(const Node& node);      ///< UTF8/Printable/IA5/BMP/T61/Universal strings
[[nodiscard]] std::string universal_name(std::uint32_t tag);
/// Generic tree as JSON ({"type","value"|"children"}), values of `secret` subtrees hidden.
[[nodiscard]] Json tree_json(const Node& node, int depth = 0);

// Encoder.
[[nodiscard]] Bytes tlv(std::uint8_t identifier, ByteView value);
[[nodiscard]] Bytes sequence(const std::vector<Bytes>& items);
[[nodiscard]] Bytes oid(std::string_view dotted);
[[nodiscard]] Bytes integer_unsigned(ByteView magnitude);  ///< big-endian magnitude -> INTEGER
[[nodiscard]] Bytes bit_string(ByteView bytes);

}  // namespace cvt::science::asn1
