// Convertible core - codec_av1 family.
// AV1 video decoder and encoder (imported libaom, generic C build, behind
// in-house wrappers), registered in registry.media() under codec::id::av1.
// Single-picture entry points for AVIF are in families/codec_av1/av1_api.hpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/codec_av1/av1_codec.hpp"
#include "families/families.hpp"

namespace cvt {

void register_codec_av1_family(Registry& registry) {
  registry.media().register_video_decoder(codec::id::av1, [] { return av1::make_decoder(); });
  registry.media().register_video_encoder(codec::id::av1, [] { return av1::make_encoder(); });
}

}  // namespace cvt
