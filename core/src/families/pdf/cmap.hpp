// Convertible core - pdf family: CMaps (character code -> CID, and ToUnicode).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::pdf {

class CMap {
 public:
  /// Parse an embedded CMap program (encoding CMap or ToUnicode CMap).
  /// `resolve_use` returns the CMap named by `usecmap` (may be null).
  [[nodiscard]] static CMap parse(ByteView program, int depth = 0);
  /// A predefined CMap by name: Identity-H/V, the Unicode ones (Uni*-UCS2-*,
  /// Uni*-UTF16-*), and anything else as a 2-byte identity mapping with
  /// `known()` false.
  [[nodiscard]] static CMap predefined(std::string_view name);

  struct Code {
    std::uint32_t value = 0;
    int length = 1;  ///< bytes
  };
  /// Split the next character code off `bytes` at `pos` using the codespace
  /// ranges (1-byte codes when there are none).
  [[nodiscard]] Code next_code(std::string_view bytes, std::size_t pos) const noexcept;
  /// CID for a code (encoding CMaps); identity when unmapped and the CMap is
  /// an identity map, otherwise 0.
  [[nodiscard]] std::uint32_t cid(Code code) const;
  /// Unicode text for a code (ToUnicode CMaps); nullopt when unmapped.
  [[nodiscard]] std::optional<std::u32string> unicode(Code code) const;

  [[nodiscard]] bool vertical() const noexcept { return vertical_; }
  [[nodiscard]] bool identity() const noexcept { return identity_; }
  /// True for predefined Unicode CMaps: the code itself is UTF-16/UCS-2.
  [[nodiscard]] bool code_is_unicode() const noexcept { return code_is_unicode_; }
  [[nodiscard]] bool known() const noexcept { return known_; }
  [[nodiscard]] bool empty() const noexcept {
    return cid_single_.empty() && cid_ranges_.empty() && uni_single_.empty() && uni_ranges_.empty();
  }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  struct Space {
    int length = 1;
    std::uint32_t low = 0;
    std::uint32_t high = 0;
  };
  struct CidRange {
    int length = 1;
    std::uint32_t low = 0, high = 0, cid = 0;
  };
  struct UniRange {
    int length = 1;
    std::uint32_t low = 0, high = 0;
    std::u32string base;                 ///< incremented in its last code point
    std::vector<std::u32string> array;   ///< or explicit values
  };
  static std::uint64_t key(int length, std::uint32_t value) noexcept {
    return (static_cast<std::uint64_t>(length) << 32) | value;
  }
  void merge(const CMap& other);

  std::vector<Space> spaces_;
  std::unordered_map<std::uint64_t, std::uint32_t> cid_single_;
  std::vector<CidRange> cid_ranges_;
  std::unordered_map<std::uint64_t, std::u32string> uni_single_;
  std::vector<UniRange> uni_ranges_;
  bool vertical_ = false;
  bool identity_ = false;
  bool code_is_unicode_ = false;
  bool known_ = true;
  std::string name_;
};

/// Decode UTF-16BE bytes (a ToUnicode destination) to code points.
[[nodiscard]] std::u32string utf16be_to_u32(std::string_view bytes);

}  // namespace cvt::pdf
