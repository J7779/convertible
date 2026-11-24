// Convertible core - audio_lossy family: registration entry points of the
// codec areas (each implemented in its own sub-directory).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::lossy {

/// MPEG-1/2/2.5 audio layers I-III: mp3/mp2/mp1 files, mp3 and mp2 codecs.
void register_mpeg_audio(Registry& registry);
/// AAC: ADTS/LOAS .aac, MPEG-4 .m4a/.m4b/.m4r/.3ga, the aac codec.
void register_aac(Registry& registry);
/// Ogg Opus, Ogg Vorbis, Ogg Speex and their codecs.
void register_xiph(Registry& registry);
/// Speech codecs: AMR-NB/WB (.amr/.awb), GSM 06.10 (.gsm).
void register_speech(Registry& registry);
/// Synthesised music: Standard MIDI Files and tracker modules.
void register_synth(Registry& registry);

}  // namespace cvt::lossy
