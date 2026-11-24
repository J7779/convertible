// Convertible core - codec_legacy family: MPEG-1 / MPEG-2 video (private).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>

#include "codec/codec.hpp"

namespace cvt::legacy::mpeg12 {

/// Decodes MPEG-1 and MPEG-2 video (the syntax is detected from the
/// bitstream; `mpeg2` only sets the default colour interpretation).
[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_decoder(bool mpeg2);
/// Encodes MPEG-1 (`mpeg2` false) or MPEG-2 Main Profile video.
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_encoder(bool mpeg2);

}  // namespace cvt::legacy::mpeg12
