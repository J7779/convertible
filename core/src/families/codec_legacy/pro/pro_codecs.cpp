// Convertible core - codec_legacy family: the production codec group (ProRes).
//
// DNxHD/DNxHR (SMPTE ST 2019-1) and DV (IEC 61834 / SMPTE 314M) are not
// implemented: their coefficient code tables and (for DV) the macroblock
// shuffling patterns are defined only in the paid standards, and no
// permissively licensed implementation exists to import.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/pro/pro.hpp"

namespace cvt::legacy {

void add_pro_codecs(std::vector<CodecEntry>& out) {
  out.push_back({codec::id::prores, [] { return pro::make_prores_decoder(); }, [] { return pro::make_prores_encoder(); }});
}

}  // namespace cvt::legacy
