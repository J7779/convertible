// Convertible core - codec_legacy family: the MPEG-1/MPEG-2 video codec group.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/mpeg12/mpeg12.hpp"

namespace cvt::legacy {

void add_mpeg12_codecs(std::vector<CodecEntry>& out) {
  out.push_back({codec::id::mpeg1video, [] { return mpeg12::make_decoder(false); }, [] { return mpeg12::make_encoder(false); }});
  out.push_back({codec::id::mpeg2video, [] { return mpeg12::make_decoder(true); }, [] { return mpeg12::make_encoder(true); }});
}

}  // namespace cvt::legacy
