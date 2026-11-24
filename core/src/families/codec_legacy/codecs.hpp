// Convertible core - codec_legacy family: the codec groups (private).
//
// Each group appends its codecs to the family table; the groups live in
// their own sub-directories and know nothing of each other.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <vector>

#include "families/codec_legacy/codec_legacy.hpp"

namespace cvt::legacy {

void add_mpeg12_codecs(std::vector<CodecEntry>& out);    // mpeg12/: mpeg1video, mpeg2video
void add_mpeg4_codecs(std::vector<CodecEntry>& out);     // mpeg4/: mpeg4, h263, flv1, msmpeg4v3
void add_theora_codecs(std::vector<CodecEntry>& out);    // theora/
void add_pro_codecs(std::vector<CodecEntry>& out);       // pro/: prores, dnxhd, dvvideo
void add_lossless_codecs(std::vector<CodecEntry>& out);  // lossless/: huffyuv, ffv1, utvideo
void add_simple_codecs(std::vector<CodecEntry>& out);    // simple/: cinepak, msrle, msvideo1, qtrle, rpza, smc
void add_raw_codecs(std::vector<CodecEntry>& out);       // raw/: uyvy, yuy2, v210, bgr24, r210

}  // namespace cvt::legacy
