// Convertible core - codec_legacy family: production codecs (private).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>

#include "codec/codec.hpp"

namespace cvt::legacy::pro {

[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_prores_decoder();
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_prores_encoder();

}  // namespace cvt::legacy::pro
