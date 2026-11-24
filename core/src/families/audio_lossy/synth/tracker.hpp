// Convertible core - audio_lossy family: the module model shared by the
// MOD/S3M/XM/IT loaders and the player.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::synth {

enum class Fx : std::uint8_t {
  None,
  Arpeggio,
  PortaUp,         ///< every tick but the first (MOD/XM 1xx, S3M/IT Fxx < E0)
  PortaDown,
  FinePortaUp,     ///< once, on the first tick
  FinePortaDown,
  ExtraFineUp,     ///< once, quarter strength
  ExtraFineDown,
  TonePorta,
  Vibrato,
  TonePortaVol,    ///< tone portamento continues + volume slide (MOD semantics)
  VibratoVol,
  Tremolo,
  SetPan,          ///< 0..255
  Offset,          ///< param * 256 frames
  VolSlide,        ///< MOD/XM semantics: x up, else y down, ticks > 0
  VolSlideS3M,     ///< S3M/IT Dxy semantics, including fine slides
  FineVolUp,
  FineVolDown,
  PosJump,
  SetVolume,       ///< 0..64
  PatBreak,        ///< row (already decimal)
  SetSpeed,
  SetTempo,
  GlobalVolume,    ///< 0..64
  GlobalVolSlide,
  Retrig,          ///< every x ticks (y = volume change, S3M/IT Qxy)
  PatLoop,
  NoteCut,
  NoteDelay,
  PatDelay,
  KeyOff,
  PanSlide,
};

/// Volume-column commands (XM and IT).
enum class VolCmd : std::uint8_t { None, Set, SlideUp, SlideDown, FineUp, FineDown, Pan, TonePorta, Vibrato };

struct Cell {
  std::uint8_t note = 0;  ///< 0 none, 1..120 note (60 = middle C), 254 cut, 255 off, 253 fade
  std::uint8_t instrument = 0;
  VolCmd vol_cmd = VolCmd::None;
  std::uint8_t vol = 0;
  Fx fx = Fx::None;
  std::uint8_t param = 0;
};

struct Pattern {
  int rows = 64;
  std::vector<Cell> cells;  ///< rows * channels
};

struct Sample {
  std::vector<float> data;
  std::size_t loop_start = 0;
  std::size_t loop_end = 0;
  int loop = 0;           ///< 0 none, 1 forward, 2 ping-pong
  int volume = 64;        ///< 0..64
  int global = 64;        ///< IT sample global volume
  double c5 = 8363.0;     ///< frequency of middle C (note 60)
  int relative = 0;       ///< pitch offset in 1/64 semitone (XM relative note + finetune)
  int pan = -1;           ///< 0..255, -1 = channel pan
};

struct Envelope {
  bool enabled = false;
  std::vector<std::pair<int, int>> points;  ///< (tick, value 0..64)
  int sustain_start = -1, sustain_end = -1;
  int loop_start = -1, loop_end = -1;
};

struct Instrument {
  std::array<std::uint8_t, 121> sample{};  ///< sample number (1-based, 0 none) per note
  std::array<std::uint8_t, 121> note{};    ///< played note per note
  Envelope volume;
  Envelope panning;
  int fadeout = 0;  ///< per tick out of 65536 (0 = none)
  int global = 64;
};

struct Module {
  std::string format;   ///< "mod", "s3m", "xm", "it"
  std::string title;
  int channels = 4;
  bool linear = false;  ///< linear frequency slides (XM/IT) instead of Amiga periods
  double clock = 14317056.0;  ///< Amiga mode: frequency = clock / period (period in 1/4 units)
  bool instruments = false;   ///< XM/IT instrument layer
  int speed = 6;
  int tempo = 125;
  int global_volume = 64;
  double mix_gain = 1.0;
  std::vector<int> orders;     ///< pattern numbers; -2 = skip (+++), -1 = end
  std::vector<Pattern> patterns;
  std::vector<Sample> samples;          ///< index 0 unused
  std::vector<Instrument> instrument_list;  ///< index 0 unused
  std::vector<int> channel_pan;         ///< 0..255 per channel
  std::vector<int> channel_volume;      ///< 0..64 per channel
  int restart = 0;
};

[[nodiscard]] Result<Module> load_mod(ByteView data, const Context& ctx);
[[nodiscard]] Result<Module> load_s3m(ByteView data, const Context& ctx);
[[nodiscard]] Result<Module> load_xm(ByteView data, const Context& ctx);
[[nodiscard]] Result<Module> load_it(ByteView data, const Context& ctx);

}  // namespace cvt::lossy::synth
