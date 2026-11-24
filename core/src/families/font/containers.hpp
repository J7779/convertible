// Convertible core - font family: font containers (WOFF, WOFF2, EOT, Mac resource forks).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

[[nodiscard]] Result<ir::Font> read_woff(ByteView data, const Context& ctx);
/// `level` 0..9 for zlib; tables that do not shrink are stored.
[[nodiscard]] Result<Bytes> write_woff(const ir::Font& font, int level, bool keep_private, const Context& ctx);

[[nodiscard]] Result<ir::Font> read_woff2(ByteView data, std::int64_t face, const Context& ctx);
struct Woff2Options {
  int level = 9;                 ///< Brotli effort 0..9
  bool transform_glyf = true;    ///< glyf/loca transform (TrueType outlines)
  bool transform_hmtx = true;    ///< hmtx transform where side bearings equal xMin
  bool keep_private = true;
};
/// Writes `font` and its `other_faces` (a collection when there are several).
[[nodiscard]] Result<Bytes> write_woff2(const ir::Font& font, const Woff2Options& options, const Context& ctx);

[[nodiscard]] Result<ir::Font> read_eot(ByteView data, const Context& ctx);
struct EotOptions {
  std::uint32_t version = 0x00020001;  ///< 0x00010000, 0x00020001 or 0x00020002
  bool obfuscate = false;              ///< XOR the font data with 0x50
};
[[nodiscard]] Result<Bytes> write_eot(const ir::Font& font, const EotOptions& options);

/// Mac resource fork (data-fork suitcase .dfont, raw resource fork,
/// AppleDouble "._" file or MacBinary): every 'sfnt' resource is a face.
[[nodiscard]] Result<ir::Font> read_resource_fonts(ByteView data, std::int64_t face, const Context& ctx);
/// A data-fork suitcase holding one 'sfnt' resource per face.
[[nodiscard]] Result<Bytes> write_dfont(const ir::Font& font);

/// Content tests used for detection.
[[nodiscard]] bool looks_like_eot(ByteView data) noexcept;
[[nodiscard]] bool looks_like_resource_fork(ByteView data) noexcept;

}  // namespace cvt::font
