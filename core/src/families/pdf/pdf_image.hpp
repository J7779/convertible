// Convertible core - pdf family: image XObjects and inline images to RGBA.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "families/pdf/interp.hpp"
#include "ir/image.hpp"

namespace cvt::pdf {

struct DecodedImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Straight RGBA. For stencil masks only the alpha channel is meaningful
  /// (the fill colour is applied by the caller).
  std::vector<std::uint8_t> rgba;
  bool has_alpha = false;
  bool stencil = false;
  bool interpolate = false;
};

/// Decode an image (XObject or inline). DCT and JPX data are decoded with
/// the registry's `jpg` / `jp2` Image readers when present; without them the
/// call fails with Status::UnsupportedConversion. `ctx` may be null (then
/// default limits apply and no registry codecs are used).
[[nodiscard]] Result<DecodedImage> decode_image(PdfFile& file, const ImageEvent& image, const Context* ctx,
                                                int depth = 0);

/// The encoded bytes of a DCT (JPEG) image XObject when it can be passed
/// through unchanged (only DCTDecode, Gray/RGB, no masks, no Decode array).
[[nodiscard]] std::optional<Bytes> passthrough_jpeg(PdfFile& file, const Object& image_stream);

/// Decode a file of another format with the registry's Image reader for
/// `format_id` ("jpg", "jp2", "png", ...). Fails with
/// Status::UnsupportedConversion when the codec is not registered.
[[nodiscard]] Result<ir::Image> decode_with_registry(const Context* ctx, std::string_view format_id, ByteView data);

/// Encode with the registry's Image writer for `format_id`.
[[nodiscard]] Result<Bytes> encode_with_registry(const Context* ctx, std::string_view format_id, const ir::Image& image,
                                                 std::string_view options_json = {});

}  // namespace cvt::pdf
