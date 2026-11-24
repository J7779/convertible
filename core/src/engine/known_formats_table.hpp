// Convertible core - row type of the known-formats database (private header).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// The rows live in known_formats_*.cpp, one file per area; the policy for
// ids, extensions, MIME types, popularity and signatures is documented at the
// top of known_formats.cpp.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "engine/registry.hpp"
#include "engine/sniffers.hpp"

namespace cvt::known {

/// A magic number: `pattern` at `offset` (grammar: see SignaturePattern).
struct KnownSignature {
  std::size_t offset = 0;
  SignaturePattern pattern{};
};

/// How to look inside the content when magic numbers are not enough.
struct KnownSniff {
  sniff::Family family = sniff::Family::None;
  /// A second classifier, for formats that come in two encodings (PEM text
  /// and DER binary certificates).
  sniff::Family also = sniff::Family::None;
  /// True when the file name may decide: the content is a valid member of
  /// the family (a ZIP, a TIFF, text ...) but carries no tell-tale of its own.
  bool named = false;

  constexpr KnownSniff() noexcept = default;
  constexpr KnownSniff(sniff::Family f) noexcept : family(f) {}  // NOLINT(google-explicit-constructor)
  constexpr KnownSniff(sniff::Family f, sniff::Family g, bool by_name) noexcept : family(f), also(g), named(by_name) {}
};

/// `family`, and the file name decides when the content is not specific.
[[nodiscard]] constexpr KnownSniff named(sniff::Family family) noexcept { return {family, sniff::Family::None, true}; }
/// Two classifiers (either may identify the format).
[[nodiscard]] constexpr KnownSniff both(sniff::Family a, sniff::Family b, bool by_name = false) noexcept {
  return {a, b, by_name};
}

/// One row of the database.
struct KnownFormat {
  std::string_view id;
  std::string_view name;
  Category category = Category::Other;
  /// Space separated, lower-case, without dots; the first is the default.
  std::string_view extensions;
  std::string_view mime;
  int popularity = 0;
  std::string_view description;
  KnownSignature signatures[8]{};
  KnownSniff sniff{};
};

// The parts of the table, one per area.
std::span<const KnownFormat> image_formats() noexcept;
std::span<const KnownFormat> media_formats() noexcept;
std::span<const KnownFormat> document_formats() noexcept;
std::span<const KnownFormat> data_formats() noexcept;
std::span<const KnownFormat> archive_formats() noexcept;
std::span<const KnownFormat> system_formats() noexcept;
std::span<const KnownFormat> code_formats() noexcept;

/// Every row of every part, in a fixed order.
std::span<const std::span<const KnownFormat>> all_parts() noexcept;

}  // namespace cvt::known
