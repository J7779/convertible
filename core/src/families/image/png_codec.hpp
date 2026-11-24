// Convertible core - image family: PNG/APNG codec interface for use inside
// the family (ICO, ICNS, the "png" video codec ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image::png {

/// Decode a PNG or APNG file (all colour types, bit depths, Adam7, tRNS,
/// gAMA, metadata chunks). Animated files yield composited full-canvas frames.
[[nodiscard]] Result<ir::Image> decode(ByteView data, const Context& ctx);

struct EncodeOptions {
  /// 0..9 DEFLATE effort; 8 and 9 also try alternative filter strategies.
  int compression_level = 6;
  /// Lossy palette quantisation (the "small" path); 0 = lossless.
  /// 1..99 = quality target (lower = fewer colours, smaller file).
  int lossy_quality = 0;
  /// Palette dithering for the lossy path.
  int dither = 1;  // 0 none, 1 Floyd-Steinberg, 2 ordered
  /// 0 = automatic, 8 or 16.
  int bit_depth = 0;
  bool interlace = false;
  /// Write EXIF/XMP/ICC/text chunks.
  bool keep_metadata = false;
};

/// Encode an image (APNG when it has several frames).
[[nodiscard]] Result<Bytes> encode(const ir::Image& image, const EncodeOptions& options, const Context& ctx);

}  // namespace cvt::image::png
