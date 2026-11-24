// Convertible core - image_modern family: lossless JPEG (ITU-T T.81 process
// 14, "LJ92") decoder as used inside DNG, CR2 and other RAW files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::raw {

/// A decoded lossless JPEG frame: `height` rows of `width * components`
/// interleaved samples (component sampling factors must all be 1).
struct LjpegFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int components = 0;
  int precision = 0;
  int predictor = 0;
  std::vector<std::uint16_t> samples;
};

/// Frame header only (cheap): dimensions, components, precision.
[[nodiscard]] Result<LjpegFrame> ljpeg_info(ByteView data);

/// Decode a complete SOI..EOI lossless JPEG stream (SOF3, Huffman coded,
/// predictors 1-7, point transform, restart intervals). `max_samples`
/// bounds the allocation; larger frames fail with Status::Limit.
[[nodiscard]] Result<LjpegFrame> decode_ljpeg(ByteView data, std::uint64_t max_samples);

}  // namespace cvt::image_modern::raw
