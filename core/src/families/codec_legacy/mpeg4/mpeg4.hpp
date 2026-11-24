// Convertible core - codec_legacy family: MPEG-4 Part 2 / H.263 / Sorenson Spark (private).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>

#include "codec/codec.hpp"

namespace cvt::legacy::mpeg4 {

enum class Flavor {
  Mpeg4,  ///< ISO/IEC 14496-2 (also accepts short-header H.263 streams)
  H263,   ///< ITU-T H.263 (baseline, PLUSPTYPE custom sizes, UMV)
  Flv1,   ///< Sorenson Spark (H.263 variant in Flash Video)
};

[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_decoder(Flavor flavor);
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_encoder(Flavor flavor);

}  // namespace cvt::legacy::mpeg4
