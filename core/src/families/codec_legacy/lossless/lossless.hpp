// Convertible core - codec_legacy family: lossless codecs (private).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>

#include "codec/codec.hpp"

namespace cvt::legacy::lossless {

[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_huffyuv_decoder();
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_huffyuv_encoder();
[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_ffv1_decoder();
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_ffv1_encoder();
[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_utvideo_decoder();

}  // namespace cvt::legacy::lossless
