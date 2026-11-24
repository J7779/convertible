// Convertible core - office family: the token tables of Microsoft Reader's
// binary markup (element and attribute numbers of HTML pages and of the
// OEB package description).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace cvt::office::lit {

struct Token {
  std::uint32_t code = 0;
  std::string_view name;
};

struct TagAttributes {
  std::uint32_t tag = 0;
  std::span<const Token> attributes;
};

class TokenMap {
 public:
  constexpr TokenMap(std::span<const std::string_view> tags, std::span<const Token> global,
                     std::span<const TagAttributes> local) noexcept
      : tags_(tags), global_(global), local_(local) {}

  /// Element name of a tag number; empty when unknown.
  [[nodiscard]] std::string_view tag(std::uint32_t code) const noexcept {
    return code < tags_.size() ? tags_[code] : std::string_view();
  }
  /// The element-specific attribute numbers of a tag.
  [[nodiscard]] std::span<const Token> attributes_of(std::uint32_t tag) const noexcept {
    for (const TagAttributes& t : local_) {
      if (t.tag == tag) return t.attributes;
    }
    return {};
  }
  /// Attribute name: the element's own numbers first, then the shared ones.
  [[nodiscard]] std::string_view attribute(std::uint32_t code, std::span<const Token> local) const noexcept {
    for (const Token& t : local) {
      if (t.code == code) return t.name;
    }
    for (const Token& t : global_) {
      if (t.code == code) return t.name;
    }
    return {};
  }

 private:
  std::span<const std::string_view> tags_;
  std::span<const Token> global_;
  std::span<const TagAttributes> local_;
};

[[nodiscard]] const TokenMap& html_tokens() noexcept;
[[nodiscard]] const TokenMap& opf_tokens() noexcept;

}  // namespace cvt::office::lit
