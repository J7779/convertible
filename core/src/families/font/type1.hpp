// Convertible core - font family: PostScript Type 1 fonts (PFA/PFB) and AFM metrics.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/font.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"

namespace cvt::font {

/// Read a Type 1 font (PFB segments or PFA text) and convert it to an
/// OpenType font with CFF outlines (hints, flex and hint replacement are
/// resolved into plain outlines; BlueValues and stem widths are kept).
[[nodiscard]] Result<ir::Font> read_type1(ByteView data, const Context& ctx);
/// Write a CFF-flavoured font as Type 1: `binary` selects PFB, else PFA.
[[nodiscard]] Result<Bytes> write_type1(const ir::Font& font, bool binary, const Context& ctx);

/// Adobe Font Metrics as structured data.
[[nodiscard]] Result<Json> read_afm(ByteView data);
/// AFM text for a font (metrics from hmtx/outlines, kerning from kern format 0).
[[nodiscard]] Result<Bytes> write_afm(const ir::Font& font, const Context& ctx);

}  // namespace cvt::font
