// Convertible core - image family: BMP/DIB decoding for use inside the
// family (ICO and CUR entries are DIBs without the file header).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image::bmp {

/// Decode a DIB: `data` starts with the BITMAPINFOHEADER (or a core/OS2/V4/V5
/// header). `pixel_offset` is the offset of the pixel data within `data`, or
/// 0 when it directly follows the header, masks and colour table.
/// `icon_mode`: the stored height covers the colour image and the AND mask
/// (ICO/CUR); only the colour image is decoded here.
[[nodiscard]] Result<ir::Image> decode_dib(ByteView data, std::size_t pixel_offset, const Context& ctx, bool icon_mode);

}  // namespace cvt::image::bmp
