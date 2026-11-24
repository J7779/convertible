// Convertible core - audio_lossy family.
// Lossy audio (mp3, aac/m4a, ogg vorbis, opus, amr, gsm, ...) and their codecs,
// plus synthesised music (MIDI, tracker modules) rendered to audio.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "families/families.hpp"

namespace cvt {

void register_audio_lossy_family(Registry& registry) {
  lossy::register_mpeg_audio(registry);
  lossy::register_aac(registry);
  lossy::register_xiph(registry);
  lossy::register_speech(registry);
  lossy::register_synth(registry);
}

}  // namespace cvt
