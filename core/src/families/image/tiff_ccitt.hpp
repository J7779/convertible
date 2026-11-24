// Convertible core - image family: CCITT fax decoding (T.4 / T.6).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::image::ccitt {

enum class Mode {
  Huffman,  ///< TIFF compression 2: modified Huffman, rows byte-aligned, no EOLs
  Group3,   ///< TIFF compression 3: T.4 with EOLs, 1D or (with two_d) mixed 1D/2D
  Group4,   ///< TIFF compression 4: T.6, 2D only
};

/// Decode `rows` lines of `width` pixels. Returns one byte per pixel: 1 for
/// black, 0 for white. Bits are read most significant first (reverse the
/// bytes first for FillOrder 2). Data that breaks off keeps the rows that
/// were decoded (the rest stay white).
[[nodiscard]] Result<std::vector<std::uint8_t>> decode(ByteView data, std::uint32_t width, std::uint32_t rows,
                                                       Mode mode, bool two_d = false, bool byte_aligned_eol = false);

}  // namespace cvt::image::ccitt
