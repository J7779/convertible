// Convertible core - codec_legacy family: the lossless codec group (HuffYUV, FFV1, Ut Video).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/lossless/lossless.hpp"

namespace cvt::legacy {

void add_lossless_codecs(std::vector<CodecEntry>& out) {
  out.push_back({id::ffv1, [] { return lossless::make_ffv1_decoder(); }, [] { return lossless::make_ffv1_encoder(); }});
  out.push_back({id::utvideo, [] { return lossless::make_utvideo_decoder(); }, {}});
  out.push_back({id::huffyuv, [] { return lossless::make_huffyuv_decoder(); }, [] { return lossless::make_huffyuv_encoder(); }});
}

}  // namespace cvt::legacy
