// Convertible core - font family: bitmap fonts (BDF, PCF, PSF, FNT/FON, EBDT/EBLC).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"

namespace cvt::font {

[[nodiscard]] Result<ir::BitmapFont> read_bdf(ByteView data, const Context& ctx);
[[nodiscard]] Result<Bytes> write_bdf(const ir::BitmapFont& font);
[[nodiscard]] Result<ir::BitmapFont> read_pcf(ByteView data, const Context& ctx);
[[nodiscard]] Result<Bytes> write_pcf(const ir::BitmapFont& font);
[[nodiscard]] Result<ir::BitmapFont> read_psf(ByteView data, const Context& ctx);
[[nodiscard]] Result<Bytes> write_psf(const ir::BitmapFont& font, const Context& ctx);
[[nodiscard]] Result<ir::BitmapFont> read_fnt(ByteView data, const Context& ctx);
/// Every FNT resource of a Windows .fon (NE executable).
[[nodiscard]] Result<std::vector<ir::BitmapFont>> read_fon(ByteView data, const Context& ctx);
/// The first strike of the EBLC/EBDT tables of a font (OpenType bitmap).
[[nodiscard]] Result<ir::BitmapFont> read_embedded_bitmaps(const ir::Font& font, const Context& ctx);

/// An outline font whose glyphs are the bitmap's pixels as squares.
[[nodiscard]] Result<ir::Font> outline_font_from_bitmap(const ir::BitmapFont& bitmap, bool cff, const Context& ctx);

[[nodiscard]] bool looks_like_fnt(ByteView data) noexcept;
[[nodiscard]] bool looks_like_fon(ByteView data) noexcept;

}  // namespace cvt::font
