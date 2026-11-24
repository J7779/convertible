// Convertible core - codec_legacy family.
// Older video codecs (mpeg1video, mpeg2video, mpeg4, h263, theora, prores, dnxhd, dv,
// huffyuv, ffv1, utvideo, cinepak, msrle, msvideo1, qtrle, rpza, smc, raw variants).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/codec_legacy/codec_legacy.hpp"
#include "families/codec_legacy/codecs.hpp"
#include "families/families.hpp"

namespace cvt {

namespace legacy {

std::vector<CodecEntry> codec_entries() {
  std::vector<CodecEntry> out;
  add_mpeg12_codecs(out);
  add_mpeg4_codecs(out);
  add_theora_codecs(out);
  add_pro_codecs(out);
  add_lossless_codecs(out);
  add_simple_codecs(out);
  add_raw_codecs(out);
  return out;
}

std::unique_ptr<codec::VideoDecoder> make_video_decoder(std::string_view codec_id) {
  for (const CodecEntry& entry : codec_entries()) {
    if (entry.id == codec_id && entry.decoder) return entry.decoder();
  }
  return nullptr;
}

std::unique_ptr<codec::VideoEncoder> make_video_encoder(std::string_view codec_id) {
  for (const CodecEntry& entry : codec_entries()) {
    if (entry.id == codec_id && entry.encoder) return entry.encoder();
  }
  return nullptr;
}

void register_codecs(Registry& registry) {
  for (CodecEntry& entry : codec_entries()) {
    // Ids that codec/ids.hpp does not know yet would be reported as
    // registration problems; they stay reachable through make_video_*().
    if (!codec::id::is_known(entry.id)) continue;
    if (entry.decoder) registry.media().register_video_decoder(entry.id, std::move(entry.decoder));
    if (entry.encoder) registry.media().register_video_encoder(entry.id, std::move(entry.encoder));
  }
}

}  // namespace legacy

void register_codec_legacy_family(Registry& registry) { legacy::register_codecs(registry); }

}  // namespace cvt
