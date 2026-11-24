// Convertible core - image_modern family: GPU block-compression decoders
// shared by the texture containers (KTX, KTX2, PVR, ASTC, VTF).
// BC1-BC5, BC7, ETC1, ETC2 (RGB, punch-through alpha, RGBA), EAC R11/RG11
// and ASTC (2D, LDR).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern {

enum class BlockFormat {
  BC1,         ///< DXT1 (1-bit alpha when colour0 <= colour1)
  BC2,         ///< DXT2/3: explicit 4-bit alpha
  BC3,         ///< DXT4/5: interpolated alpha
  BC4,         ///< one channel (unsigned), shown as grey
  BC4Signed,
  BC5,         ///< two channels (unsigned) -> R, G
  BC5Signed,
  BC7,
  ETC1,
  ETC2,        ///< ETC2 RGB (ETC1 is a subset)
  ETC2A1,      ///< ETC2 RGB with punch-through alpha
  ETC2EAC,     ///< ETC2 RGB + EAC alpha
  EACR11,
  EACR11Signed,
  EACRG11,
  EACRG11Signed,
  ASTC,        ///< ASTC LDR; the footprint is given separately
};

struct BlockShape {
  int width = 4;
  int height = 4;
  int bytes = 8;
};

[[nodiscard]] BlockShape block_shape(BlockFormat format, int astc_width = 4, int astc_height = 4) noexcept;
[[nodiscard]] const char* block_format_name(BlockFormat format) noexcept;

/// Decode one block into `out` (`shape.width * shape.height` RGBA pixels,
/// row-major). Returns false for an invalid block (decoded as magenta, the
/// ASTC error colour, for ASTC; never for the other formats).
bool decode_block(BlockFormat format, const std::uint8_t* block, std::uint8_t* out, int astc_width = 4,
                  int astc_height = 4) noexcept;

/// Decode a whole `width` x `height` surface stored as consecutive blocks
/// (row-major block order). Fails when `data` is too short. Progress and
/// cancellation go through `ctx`.
[[nodiscard]] Result<ir::Frame> decode_surface(BlockFormat format, ByteView data, std::uint32_t width,
                                               std::uint32_t height, const Context& ctx, int astc_width = 4,
                                               int astc_height = 4);

/// Bytes a surface needs (overflow-checked: 0 when absurd).
[[nodiscard]] std::uint64_t surface_bytes(BlockFormat format, std::uint32_t width, std::uint32_t height,
                                          int astc_width = 4, int astc_height = 4) noexcept;

}  // namespace cvt::image_modern
