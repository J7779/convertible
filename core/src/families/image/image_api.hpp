// Convertible core - image family: decode/encode any registered image format.
//
// Free functions for families that embed pictures (PDF, office documents,
// video containers, meshes, icons ...): they run the registered `ir::Image`
// reader or writer of a format through the registry, exactly as the engine
// would, with the caller's limits, progress, cancellation and warnings.
//
//   #include "families/image/image_api.hpp"
//   CVT_TRY_ASSIGN(ir::Image picture, image::decode("jpg", embedded_bytes, ctx));
//   CVT_TRY_ASSIGN(Bytes png, image::encode("png", picture, Options{}, ctx));
//
// Formats of other families (webp, avif, ...) work the same way once their
// family is registered; when no codec exists the result is
// Status::UnsupportedConversion with a clear message, so callers can degrade
// gracefully.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}

namespace cvt::image {

/// Decode `bytes` as `format_id` ("png", "jpg", "gif", "tiff", ...).
/// `ctx.registry` is used when set, else the shared registry. `options` are
/// passed to the reader (normalised against its specs). Progress is reported
/// into `ctx.progress`; warnings go to `ctx.warnings`.
[[nodiscard]] Result<ir::Image> decode(std::string_view format_id, ByteView bytes, const Context& ctx,
                                       const Options& options = Options{});

/// Encode `image` as `format_id` with writer options (`quality`,
/// `compression_level`, `strip_metadata` ...).
[[nodiscard]] Result<Bytes> encode(std::string_view format_id, const ir::Image& image, const Options& options,
                                   const Context& ctx);

/// True when `format_id` has a registered Image reader / writer.
[[nodiscard]] bool can_decode(std::string_view format_id, const Context& ctx);
[[nodiscard]] bool can_encode(std::string_view format_id, const Context& ctx);

/// The id of the image format the bytes start with, from the signatures of
/// the common raster formats (png, jpg, gif, bmp, tiff, webp, ico, qoi,
/// psd ...), or nullopt. Content only; no file name needed.
[[nodiscard]] std::optional<std::string> sniff_format(ByteView bytes);

}  // namespace cvt::image
