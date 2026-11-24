// Convertible core - codec_hevc family.
// H.265/HEVC video decoder (libhevc) and encoder (Kvazaar), registered in
// registry.media() under codec::id::hevc, and the raw "hevc" stream format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/codec_hevc/hevc_internal.hpp"
#include "families/families.hpp"

namespace cvt {

void register_codec_hevc_family(Registry& registry) {
  registry.media().register_video_decoder(codec::id::hevc, [] { return hevc::make_decoder(); });
  registry.media().register_video_encoder(codec::id::hevc, [] { return hevc::make_encoder(); });
  hevc::register_stream_format(registry);
}

}  // namespace cvt
