// Convertible core - codec_legacy family: the MPEG-4 Part 2 / H.263 codec group.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/mpeg4/mpeg4.hpp"

namespace cvt::legacy {

void add_mpeg4_codecs(std::vector<CodecEntry>& out) {
  out.push_back({codec::id::mpeg4, [] { return mpeg4::make_decoder(mpeg4::Flavor::Mpeg4); },
                 [] { return mpeg4::make_encoder(mpeg4::Flavor::Mpeg4); }});
  out.push_back({codec::id::h263, [] { return mpeg4::make_decoder(mpeg4::Flavor::H263); },
                 [] { return mpeg4::make_encoder(mpeg4::Flavor::H263); }});
  out.push_back({id::flv1, [] { return mpeg4::make_decoder(mpeg4::Flavor::Flv1); },
                 [] { return mpeg4::make_encoder(mpeg4::Flavor::Flv1); }});
}

}  // namespace cvt::legacy
