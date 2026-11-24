// Convertible core - image_modern family: WebP reader and writer (private).
// libwebp's own headers are included only by the webp*.cpp files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::webp {

/// Largest width or height a WebP bitstream can hold (WEBP_MAX_DIMENSION).
inline constexpr std::uint32_t max_dimension = 16383;

/// Run libwebp's one-time initialisers (DSP function tables, lookup tables)
/// exactly once, under a C++ function-local static. libwebp built without
/// threads guards them with unsynchronised flags, so without this two
/// conversions starting at the same time could race. Every entry point calls
/// it before touching libwebp.
void ensure_initialised();

/// Decode a WebP file (still or animated) into an image.
[[nodiscard]] Result<ir::Image> read_webp(ReadContext& ctx);
/// Encode an image (one frame: still; several: animation).
[[nodiscard]] Result<Bytes> write_webp(const ir::Image& image, WriteContext& ctx);
/// The writer's options.
[[nodiscard]] std::vector<OptionSpec> write_options();

}  // namespace cvt::image_modern::webp
