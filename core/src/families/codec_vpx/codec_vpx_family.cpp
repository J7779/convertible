// Convertible core - codec_vpx family.
// VP8 and VP9 video decoders and encoders (libvpx, generic C build).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "families/families.hpp"

namespace cvt {

void register_codec_vpx_family(Registry& registry) {
  codec::Registry& media = registry.media();
  media.register_video_decoder(codec::id::vp8, [] { return vpx::make_decoder(vpx::Codec::Vp8); });
  media.register_video_decoder(codec::id::vp9, [] { return vpx::make_decoder(vpx::Codec::Vp9); });
  media.register_video_encoder(codec::id::vp8, [] { return vpx::make_encoder(vpx::Codec::Vp8); });
  media.register_video_encoder(codec::id::vp9, [] { return vpx::make_encoder(vpx::Codec::Vp9); });
}

}  // namespace cvt
