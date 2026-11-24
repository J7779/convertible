// Convertible core - codec_h264 family.
// H.264/AVC video decoder and encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <memory>

#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/codec_h264/h264_codec.hpp"
#include "families/families.hpp"

namespace cvt {

namespace h264 {

void register_codecs(Registry& registry) {
  registry.media().register_video_decoder(codec::id::h264, [] { return std::make_unique<H264Decoder>(); });
  registry.media().register_video_encoder(codec::id::h264, [] { return std::make_unique<H264Encoder>(); });
}

}  // namespace h264

void register_codec_h264_family(Registry& registry) {
  h264::register_codecs(registry);
}

}  // namespace cvt
