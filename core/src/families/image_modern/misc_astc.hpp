// Convertible core - image_modern family: ASTC block decoder (2D, LDR).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

namespace cvt::image_modern {

/// Decode one 16-byte ASTC block of `width` x `height` texels (4..12 each)
/// into row-major RGBA8 `out`. Invalid blocks and HDR endpoint modes give
/// the ASTC error colour (magenta) and return false.
bool decode_astc_block(const std::uint8_t* block, int width, int height, std::uint8_t* out) noexcept;

}  // namespace cvt::image_modern
