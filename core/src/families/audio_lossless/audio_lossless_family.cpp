// Convertible core - audio_lossless family.
// Uncompressed and lossless audio (wav, aiff, au, flac, alac, wavpack, ...) and their codecs,
// plus the audio DSP toolbox (audio_dsp.hpp) and helpers for other families (audio_api.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/families.hpp"

namespace cvt {

void register_audio_lossless_family(Registry& registry) {
  audio::register_wav(registry);
  audio::register_aiff(registry);
  audio::register_au(registry);
  audio::register_raw(registry);
  audio::register_voc(registry);
  audio::register_svx(registry);
  audio::register_sphere(registry);
  audio::register_flac(registry);
  audio::register_m4a(registry);
  audio::register_caf(registry);
  audio::register_wavpack(registry);
  audio::register_tta(registry);
  audio::register_media_codecs(registry);
  audio::register_bridges(registry);
}

}  // namespace cvt
