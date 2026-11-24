// Convertible core - audio_lossy family: synthesised music (Standard MIDI
// Files with a built-in General MIDI synthesiser; MOD/S3M/XM/IT modules).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::synth {

struct RenderSettings {
  std::uint32_t sample_rate = 44100;
  double max_seconds = 600.0;
};

// ------------------------------------------------------------------ MIDI

struct MidiEvent {
  std::uint64_t tick = 0;   ///< absolute tick in its track
  double seconds = 0.0;     ///< absolute time from the tempo map
  int track = 0;
  std::uint8_t status = 0;  ///< 0x80..0xEF channel message, 0xFF meta, 0xF0/0xF7 sysex
  std::uint8_t meta = 0;    ///< meta type when status == 0xFF
  std::vector<std::uint8_t> data;
};

struct MidiFile {
  int format = 0;
  int tracks = 0;
  int division = 480;  ///< ticks per quarter note (or SMPTE when negative)
  std::vector<MidiEvent> events;  ///< all tracks, sorted by time (stable)
  double duration = 0.0;
  std::string title;
  std::string copyright;
};

[[nodiscard]] Result<MidiFile> parse_midi(ByteView data, const Context& ctx);
[[nodiscard]] Result<ir::Audio> render_midi(const MidiFile& midi, const RenderSettings& settings, const Context& ctx);
[[nodiscard]] bool looks_like_midi(ByteView data);

// ------------------------------------------------------------------ modules

[[nodiscard]] Result<ir::Audio> render_module(ByteView data, std::string_view format, const RenderSettings& settings,
                                              const Context& ctx);
/// 0..100 detection confidence for "mod", "s3m", "xm", "it".
[[nodiscard]] int sniff_module(ByteView data, std::string_view format);

}  // namespace cvt::lossy::synth
