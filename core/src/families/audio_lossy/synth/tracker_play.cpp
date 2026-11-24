// Convertible core - audio_lossy family: the module player shared by MOD,
// S3M, XM and IT: tick sequencing (speed/tempo, pattern jumps, breaks,
// loops and delays, end-of-song detection), Amiga-period and linear pitch,
// the common effects, volume/panning envelopes with sustain, loops and
// fade-out, and a mixer with linear interpolation and forward/ping-pong loops.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossy/synth/synth.hpp"
#include "families/audio_lossy/synth/tracker.hpp"

namespace cvt::lossy::synth {

namespace {

/// ProTracker's vibrato/tremolo sine (0..255 over half a period).
constexpr std::array<int, 32> sine_table = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                            224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                            212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

struct Chan {
  const Sample* sample = nullptr;
  const Instrument* instrument = nullptr;
  double pos = 0.0;
  bool backwards = false;
  bool playing = false;
  int note = 0;
  double pitch = 0.0;   ///< Amiga: period (quarter units); linear: 1/64 semitones
  double target = 0.0;
  int volume = 64;
  int pan = 128;
  int channel_volume = 64;
  // effect state
  std::uint8_t mem_porta = 0, mem_tone = 0, mem_vib = 0, mem_trem = 0, mem_vol = 0, mem_offset = 0, mem_retrig = 0,
               mem_fine = 0, mem_global = 0;
  int vib_pos = 0, trem_pos = 0;
  double pitch_offset = 0.0;
  int volume_offset = 0;
  // envelopes
  bool key_on = true;
  int env_tick = 0;
  int pan_env_tick = 0;
  int fade = 65536;
  // row processing
  Cell cell;
  int loop_row = 0;
  int loop_count = 0;
  int retrig_tick = 0;
};

class Player {
 public:
  Player(const Module& m, double rate) : m_(m), rate_(rate), chans_(static_cast<std::size_t>(m.channels)) {
    speed_ = m.speed;
    tempo_ = m.tempo;
    global_ = m.global_volume;
    for (std::size_t c = 0; c < chans_.size(); ++c) {
      chans_[c].pan = c < m.channel_pan.size() ? m.channel_pan[c] : 128;
      chans_[c].channel_volume = c < m.channel_volume.size() ? m.channel_volume[c] : 64;
    }
    visited_.assign(m.orders.size() * 256, false);
  }

  /// Renders the next tick; false at the end of the song.
  bool tick(std::vector<float>& out) {
    if (tick_ == 0 && !repeating_) {
      if (!start_row()) return false;
    }
    process_tick();
    mix(out);
    if (++tick_ >= speed_) {
      tick_ = 0;
      repeating_ = false;
      if (delay_left_ > 0) {
        --delay_left_;
        if (delay_left_ > 0) {
          repeating_ = true;  // pattern delay: play the row again without new notes
          return true;
        }
      }
      advance();
    }
    return true;
  }

 private:
  const Pattern* pattern() const {
    const int p = m_.orders[static_cast<std::size_t>(order_)];
    return p >= 0 && p < static_cast<int>(m_.patterns.size()) ? &m_.patterns[static_cast<std::size_t>(p)] : nullptr;
  }

  bool start_row() {
    // Skip markers and missing patterns.
    for (int guard = 0; guard < 512; ++guard) {
      if (order_ >= static_cast<int>(m_.orders.size())) return false;
      if (m_.orders[static_cast<std::size_t>(order_)] == -1) return false;
      if (pattern() != nullptr) break;
      ++order_;
      row_ = 0;
    }
    if (order_ >= static_cast<int>(m_.orders.size())) return false;
    const Pattern* p = pattern();
    if (p == nullptr) return false;
    if (row_ >= p->rows) row_ = 0;
    const std::size_t key = static_cast<std::size_t>(order_) * 256 + static_cast<std::size_t>(row_);
    if (visited_[key]) return false;  // the song loops: stop here
    visited_[key] = true;
    jump_order_ = -1;
    break_row_ = -1;
    loop_jump_ = -1;
    for (std::size_t c = 0; c < chans_.size(); ++c) {
      chans_[c].cell = p->cells[static_cast<std::size_t>(row_) * chans_.size() + c];
    }
    return true;
  }

  void advance() {
    const Pattern* p = pattern();
    if (loop_jump_ >= 0) {
      // Pattern loop: replaying rows is intended, so forget them.
      for (int r = loop_jump_; r <= row_; ++r) visited_[static_cast<std::size_t>(order_) * 256 + static_cast<std::size_t>(r)] = false;
      row_ = loop_jump_;
      return;
    }
    if (jump_order_ >= 0 || break_row_ >= 0) {
      order_ = jump_order_ >= 0 ? jump_order_ : order_ + 1;
      row_ = std::max(0, break_row_);
      return;
    }
    if (++row_ >= (p != nullptr ? p->rows : 64)) {
      row_ = 0;
      ++order_;
    }
  }

  double note_pitch(const Chan& c, int note) const {
    const Sample& s = *c.sample;
    if (m_.linear) return note * 64.0 + s.relative;
    const double freq = s.c5 * std::pow(2.0, (note - 60 + s.relative / 64.0) / 12.0);
    return m_.clock / std::max(freq, 1.0);
  }

  double frequency(const Chan& c) const {
    const double p = c.pitch + c.pitch_offset;
    if (m_.linear) return c.sample->c5 * std::pow(2.0, (p / 64.0 - 60.0) / 12.0);
    return p > 1.0 ? m_.clock / p : 0.0;
  }

  void slide(Chan& c, double amount) {  // positive = up in pitch
    if (m_.linear) c.pitch += amount;
    else c.pitch = std::clamp(c.pitch - amount, 32.0, 65535.0);
  }

  void trigger(Chan& c, const Cell& cell) {
    const bool tone_porta = cell.fx == Fx::TonePorta || cell.fx == Fx::TonePortaVol || cell.vol_cmd == VolCmd::TonePorta;
    if (cell.instrument != 0) {
      int sample = cell.instrument;
      c.instrument = nullptr;
      if (m_.instruments && cell.instrument < m_.instrument_list.size()) {
        const Instrument& ins = m_.instrument_list[cell.instrument];
        const int n = cell.note >= 1 && cell.note <= 120 ? cell.note : std::clamp(c.note, 1, 120);
        if (ins.sample[static_cast<std::size_t>(n)] != 0) {
          c.instrument = &ins;
          sample = ins.sample[static_cast<std::size_t>(n)];
        } else if (m_.format == "xm") {
          sample = 0;
        }
      }
      if (sample > 0 && static_cast<std::size_t>(sample) < m_.samples.size() && !m_.samples[static_cast<std::size_t>(sample)].data.empty()) {
        if (!tone_porta || c.sample == nullptr) c.sample = &m_.samples[static_cast<std::size_t>(sample)];
        c.volume = c.sample->volume;
        if (c.sample->pan >= 0) c.pan = c.sample->pan;
        c.key_on = true;
        c.env_tick = 0;
        c.pan_env_tick = 0;
        c.fade = 65536;
      }
    }
    if (cell.note >= 1 && cell.note <= 120) {
      int note = cell.note;
      if (c.instrument != nullptr) note = c.instrument->note[static_cast<std::size_t>(note)];
      if (c.sample != nullptr) {
        if (tone_porta && c.playing) {
          c.target = note_pitch(c, note);
        } else {
          c.note = cell.note;
          c.pitch = note_pitch(c, note);
          c.target = c.pitch;
          c.pos = 0.0;
          c.backwards = false;
          c.playing = true;
          c.vib_pos = 0;
          c.trem_pos = 0;
          c.key_on = true;
          c.env_tick = 0;
          c.pan_env_tick = 0;
          c.fade = 65536;
        }
      }
    } else if (cell.note == 254) {
      c.playing = false;
    } else if (cell.note == 255) {
      c.key_on = false;
      if (c.instrument == nullptr || !c.instrument->volume.enabled) {
        if (m_.format == "xm") c.volume = 0;
        else c.playing = c.instrument != nullptr ? c.playing : false;
      }
    } else if (cell.note == 253) {
      c.key_on = false;
    }
    switch (cell.vol_cmd) {
      case VolCmd::Set: c.volume = cell.vol; break;
      case VolCmd::Pan: c.pan = cell.vol; break;
      case VolCmd::FineUp: c.volume = std::min(64, c.volume + cell.vol); break;
      case VolCmd::FineDown: c.volume = std::max(0, c.volume - cell.vol); break;
      case VolCmd::TonePorta:
        if (cell.vol != 0) c.mem_tone = cell.vol;
        break;
      default: break;
    }
  }

  void volume_slide_s3m(Chan& c, std::uint8_t param, bool first) {
    const int x = param >> 4, y = param & 15;
    if (y == 0xF && x != 0) {
      if (first) c.volume = std::min(64, c.volume + x);
    } else if (x == 0xF && y != 0) {
      if (first) c.volume = std::max(0, c.volume - y);
    } else if (!first) {
      if (x != 0) c.volume = std::min(64, c.volume + x);
      else c.volume = std::max(0, c.volume - y);
    }
  }

  void process_tick() {
    const bool first = tick_ == 0;
    for (Chan& c : chans_) {
      const Cell& cell = c.cell;
      std::uint8_t p = cell.param;
      if (first) {
        c.pitch_offset = 0.0;
        c.volume_offset = 0;
        if (!(cell.fx == Fx::NoteDelay && p > 0) && !repeating_) trigger(c, cell);
      } else if (cell.fx == Fx::NoteDelay && p == tick_) {
        trigger(c, cell);
      }
      // Volume column, every tick but the first.
      if (!first) {
        if (cell.vol_cmd == VolCmd::SlideUp) c.volume = std::min(64, c.volume + cell.vol);
        if (cell.vol_cmd == VolCmd::SlideDown) c.volume = std::max(0, c.volume - cell.vol);
        if (cell.vol_cmd == VolCmd::TonePorta) tone_porta(c);
      }
      switch (cell.fx) {
        case Fx::Arpeggio: {
          if (p != 0) c.mem_fine = p;
          const int step = tick_ % 3;
          const int semis = step == 0 ? 0 : step == 1 ? (c.mem_fine >> 4) : (c.mem_fine & 15);
          c.pitch_offset = 0.0;
          if (semis != 0) {
            if (m_.linear) c.pitch_offset = semis * 64.0;
            else c.pitch_offset = c.pitch * (std::pow(2.0, -semis / 12.0) - 1.0);
          }
          break;
        }
        case Fx::PortaUp:
        case Fx::PortaDown:
          if (p != 0) c.mem_porta = p;
          if (!first) slide(c, (cell.fx == Fx::PortaUp ? 4.0 : -4.0) * c.mem_porta);
          break;
        case Fx::FinePortaUp:
        case Fx::FinePortaDown:
          if (first) slide(c, (cell.fx == Fx::FinePortaUp ? 4.0 : -4.0) * p);
          break;
        case Fx::ExtraFineUp:
        case Fx::ExtraFineDown:
          if (first) slide(c, (cell.fx == Fx::ExtraFineUp ? 1.0 : -1.0) * p);
          break;
        case Fx::TonePorta:
          if (p != 0) c.mem_tone = p;
          if (!first) tone_porta(c);
          break;
        case Fx::TonePortaVol:
          if (!first) {
            tone_porta(c);
            mod_volume_slide(c, p);
          }
          break;
        case Fx::Vibrato:
          if ((p & 15) != 0) c.mem_vib = static_cast<std::uint8_t>((c.mem_vib & 0xF0) | (p & 15));
          if ((p >> 4) != 0) c.mem_vib = static_cast<std::uint8_t>((c.mem_vib & 0x0F) | (p & 0xF0));
          vibrato(c, first);
          break;
        case Fx::VibratoVol:
          vibrato(c, first);
          if (!first) mod_volume_slide(c, p);
          break;
        case Fx::Tremolo: {
          if ((p & 15) != 0) c.mem_trem = static_cast<std::uint8_t>((c.mem_trem & 0xF0) | (p & 15));
          if ((p >> 4) != 0) c.mem_trem = static_cast<std::uint8_t>((c.mem_trem & 0x0F) | (p & 0xF0));
          const int depth = c.mem_trem & 15;
          int v = sine_table[static_cast<std::size_t>(c.trem_pos & 31)];
          if ((c.trem_pos & 32) != 0) v = -v;
          c.volume_offset = v * depth / 64;
          if (!first) c.trem_pos = (c.trem_pos + (c.mem_trem >> 4)) & 63;
          break;
        }
        case Fx::SetPan:
          if (first) c.pan = p;
          break;
        case Fx::Offset:
          if (first) {
            if (p != 0) c.mem_offset = p;
            if (c.sample != nullptr && cell.note >= 1 && cell.note <= 120) {
              c.pos = c.mem_offset * 256.0;
              if (c.pos >= static_cast<double>(c.sample->data.size())) c.playing = c.sample->loop != 0, c.pos = static_cast<double>(c.sample->loop_start);
            }
          }
          break;
        case Fx::VolSlide:
          if (!first) mod_volume_slide(c, p);
          break;
        case Fx::VolSlideS3M:
          if (p != 0) c.mem_vol = p;
          volume_slide_s3m(c, c.mem_vol, first);
          break;
        case Fx::FineVolUp:
          if (first) c.volume = std::min(64, c.volume + p);
          break;
        case Fx::FineVolDown:
          if (first) c.volume = std::max(0, c.volume - p);
          break;
        case Fx::PosJump:
          if (first) {
            jump_order_ = p;
            if (break_row_ < 0) break_row_ = 0;
          }
          break;
        case Fx::SetVolume:
          if (first) c.volume = std::min<int>(64, p);
          break;
        case Fx::PatBreak:
          if (first) break_row_ = p;
          break;
        case Fx::SetSpeed:
          if (first && p > 0) speed_ = p;
          break;
        case Fx::SetTempo:
          if (first && p >= 32) tempo_ = p;
          break;
        case Fx::GlobalVolume:
          if (first) global_ = std::min<int>(64, p);
          break;
        case Fx::GlobalVolSlide:
          if (p != 0) c.mem_global = p;
          if (!first) {
            const int x = c.mem_global >> 4, y = c.mem_global & 15;
            global_ = std::clamp(global_ + (x != 0 ? x : -y), 0, 64);
          }
          break;
        case Fx::Retrig: {
          if (p != 0) c.mem_retrig = p;
          const int interval = c.mem_retrig & 15;
          if (interval > 0 && !first && tick_ % interval == 0 && c.sample != nullptr) {
            c.pos = 0.0;
            c.backwards = false;
            c.playing = true;
          }
          break;
        }
        case Fx::PatLoop:
          if (first) {
            if (p == 0) {
              c.loop_row = row_;
            } else if (c.loop_count == 0) {
              c.loop_count = p;
              loop_jump_ = c.loop_row;
            } else if (--c.loop_count > 0) {
              loop_jump_ = c.loop_row;
            }
          }
          break;
        case Fx::NoteCut:
          if (tick_ == p) c.volume = 0;
          break;
        case Fx::PatDelay:
          if (first && !repeating_ && delay_left_ == 0 && p > 0) delay_left_ = p + 1;
          break;
        case Fx::KeyOff:
          if (tick_ == p) c.key_on = false;
          break;
        case Fx::PanSlide:
          if (!first) c.pan = std::clamp(c.pan + ((p >> 4) != 0 ? (p >> 4) : -(p & 15)) * 4, 0, 255);
          break;
        default: break;
      }
      if (cell.vol_cmd == VolCmd::Vibrato && cell.fx != Fx::Vibrato) vibrato(c, first);
    }
  }

  void mod_volume_slide(Chan& c, std::uint8_t p) {
    if (p != 0) c.mem_vol = p;
    const int x = c.mem_vol >> 4, y = c.mem_vol & 15;
    c.volume = std::clamp(c.volume + (x != 0 ? x : -y), 0, 64);
  }

  void tone_porta(Chan& c) {
    const double speed = 4.0 * c.mem_tone;
    if (m_.linear) {
      if (c.pitch < c.target) c.pitch = std::min(c.target, c.pitch + speed);
      else c.pitch = std::max(c.target, c.pitch - speed);
    } else {
      if (c.pitch > c.target) c.pitch = std::max(c.target, c.pitch - speed);
      else c.pitch = std::min(c.target, c.pitch + speed);
    }
  }

  void vibrato(Chan& c, bool first) {
    const int depth = c.mem_vib & 15;
    int v = sine_table[static_cast<std::size_t>(c.vib_pos & 31)];
    if ((c.vib_pos & 32) != 0) v = -v;
    const double amount = v * depth / 128.0 * 4.0;  // quarter-period units
    c.pitch_offset = m_.linear ? amount : -amount;
    if (!first) c.vib_pos = (c.vib_pos + (c.mem_vib >> 4)) & 63;
  }

  static double envelope_value(const Envelope& e, int& tick, bool key_on) {
    if (!e.enabled || e.points.empty()) return 1.0;
    const auto& pts = e.points;
    // Sustain / loop handling on the tick counter.
    if (key_on && e.sustain_start >= 0 && e.sustain_end < static_cast<int>(pts.size())) {
      const int end = pts[static_cast<std::size_t>(e.sustain_end)].first;
      if (tick > end) tick = pts[static_cast<std::size_t>(e.sustain_start)].first;
    } else if (e.loop_start >= 0 && e.loop_end < static_cast<int>(pts.size()) && e.loop_start <= e.loop_end) {
      const int end = pts[static_cast<std::size_t>(e.loop_end)].first;
      if (tick > end) tick = pts[static_cast<std::size_t>(e.loop_start)].first;
    }
    const int t = tick;
    if (t <= pts.front().first) return pts.front().second / 64.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      if (t <= pts[i].first) {
        const auto [x0, y0] = pts[i - 1];
        const auto [x1, y1] = pts[i];
        const double f = x1 > x0 ? static_cast<double>(t - x0) / (x1 - x0) : 1.0;
        return (y0 + (y1 - y0) * f) / 64.0;
      }
    }
    return pts.back().second / 64.0;
  }

  void mix(std::vector<float>& out) {
    const auto frames = static_cast<std::size_t>(std::max(1.0, std::round(rate_ * 2.5 / tempo_)));
    const std::size_t base = out.size();
    out.resize(base + frames * 2, 0.0f);
    const double master = m_.mix_gain * 2.0 / std::max(4, static_cast<int>(chans_.size()));
    for (Chan& c : chans_) {
      // Envelopes and fade-out advance once per tick.
      double env = 1.0;
      double pan = c.pan;
      if (c.instrument != nullptr) {
        env = envelope_value(c.instrument->volume, c.env_tick, c.key_on);
        ++c.env_tick;
        if (c.instrument->panning.enabled) {
          pan = std::clamp(c.pan + (envelope_value(c.instrument->panning, c.pan_env_tick, c.key_on) - 0.5) * 255.0, 0.0, 255.0);
          ++c.pan_env_tick;
        }
        if (!c.key_on && (c.instrument->volume.enabled || m_.format == "it")) {
          c.fade = std::max(0, c.fade - c.instrument->fadeout);
          if (c.instrument->fadeout == 0 && !c.instrument->volume.enabled && m_.format == "it") c.fade = 0;
          if (c.fade == 0) c.playing = false;
        }
        if (c.instrument->volume.enabled && c.instrument->volume.points.size() > 0 && !c.key_on &&
            env <= 0.0 && c.env_tick > c.instrument->volume.points.back().first) {
          c.playing = false;
        }
      }
      if (!c.playing || c.sample == nullptr) continue;
      const Sample& s = *c.sample;
      const double freq = frequency(c);
      if (freq <= 0.0) continue;
      const double step = freq / rate_;
      const double vol = std::clamp(c.volume + c.volume_offset, 0, 64) / 64.0 * env * (c.fade / 65536.0) * (s.global / 64.0) *
                         (c.instrument != nullptr ? c.instrument->global / 64.0 : 1.0) * (c.channel_volume / 64.0) *
                         (global_ / 64.0) * master;
      const double gl = vol * std::cos(pan / 255.0 * std::numbers::pi / 2);
      const double gr = vol * std::sin(pan / 255.0 * std::numbers::pi / 2);
      const std::size_t n = s.data.size();
      for (std::size_t i = 0; i < frames && c.playing; ++i) {
        const auto ip = static_cast<std::size_t>(c.pos);
        const double frac = c.pos - static_cast<double>(ip);
        const float a = s.data[std::min(ip, n - 1)];
        std::size_t next = ip + 1;
        if (s.loop != 0 && next >= s.loop_end) next = s.loop == 1 ? s.loop_start : s.loop_end - 1;
        const float b = next < n ? s.data[next] : 0.0f;
        const double v = a + (b - a) * frac;
        out[base + 2 * i] += static_cast<float>(v * gl);
        out[base + 2 * i + 1] += static_cast<float>(v * gr);
        c.pos += c.backwards ? -step : step;
        if (s.loop == 0) {
          if (c.pos >= static_cast<double>(n)) c.playing = false;
        } else if (s.loop == 1) {
          if (c.pos >= static_cast<double>(s.loop_end)) {
            const double len = static_cast<double>(s.loop_end - s.loop_start);
            c.pos = static_cast<double>(s.loop_start) + std::fmod(c.pos - static_cast<double>(s.loop_start), len);
          }
        } else {
          if (!c.backwards && c.pos >= static_cast<double>(s.loop_end)) {
            c.pos = 2.0 * static_cast<double>(s.loop_end) - c.pos - 1.0;
            c.backwards = true;
          } else if (c.backwards && c.pos < static_cast<double>(s.loop_start)) {
            c.pos = 2.0 * static_cast<double>(s.loop_start) - c.pos;
            c.backwards = false;
          }
          c.pos = std::clamp(c.pos, static_cast<double>(s.loop_start), static_cast<double>(s.loop_end) - 1.0);
        }
      }
    }
    for (std::size_t i = base; i < out.size(); ++i) out[i] = static_cast<float>(std::tanh(out[i]));
  }

  const Module& m_;
  double rate_;
  std::vector<Chan> chans_;
  int speed_ = 6;
  int tempo_ = 125;
  int global_ = 64;
  int order_ = 0;
  int row_ = 0;
  int tick_ = 0;
  int delay_left_ = 0;
  bool repeating_ = false;
  int jump_order_ = -1;
  int break_row_ = -1;
  int loop_jump_ = -1;
  std::vector<bool> visited_;
};

}  // namespace

int sniff_module(ByteView data, std::string_view format) {
  const auto d = as_u8(data);
  if (format == "xm") return d.size() >= 60 && std::memcmp(d.data(), "Extended Module: ", 17) == 0 ? 98 : 0;
  if (format == "it") return d.size() >= 192 && std::memcmp(d.data(), "IMPM", 4) == 0 ? 96 : 0;
  if (format == "s3m") return d.size() >= 96 && std::memcmp(d.data() + 44, "SCRM", 4) == 0 ? 96 : 0;
  if (format == "mod" && d.size() >= 1084) {
    const std::string_view sig(reinterpret_cast<const char*>(d.data() + 1080), 4);
    if (sig == "M.K." || sig == "M!K!" || sig == "FLT4" || sig == "FLT8" || sig == "4CHN" || sig == "6CHN" || sig == "8CHN" ||
        sig == "CD81" || sig == "OKTA" || ((sig.substr(2) == "CH" || sig.substr(2) == "CN") && sig[0] >= '0' && sig[0] <= '9')) {
      return 90;
    }
  }
  return 0;
}

Result<ir::Audio> render_module(ByteView data, std::string_view format, const RenderSettings& settings, const Context& ctx) {
  Result<Module> loaded = format == "s3m" ? load_s3m(data, ctx)
                          : format == "xm" ? load_xm(data, ctx)
                          : format == "it" ? load_it(data, ctx)
                                           : load_mod(data, ctx);
  if (!loaded) return std::unexpected(std::move(loaded).error());
  const Module& m = *loaded;
  if (m.orders.empty() || m.patterns.empty()) return fail(Status::Decode, "the module has no patterns to play");
  Player player(m, settings.sample_rate);
  ir::Audio audio;
  audio.sample_rate = settings.sample_rate;
  audio.channels = 2;
  const auto max_frames = static_cast<std::uint64_t>(settings.max_seconds * settings.sample_rate);
  CVT_TRY(audio::check_sample_budget(ctx, 0));
  std::size_t ticks = 0;
  while (audio.frame_count() < max_frames) {
    if (!player.tick(audio.samples)) break;
    if ((++ticks & 255) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(audio.frame_count()) / static_cast<double>(max_frames), "Rendering"));
      CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
    }
  }
  if (audio.frame_count() >= max_frames) {
    audio.samples.resize(static_cast<std::size_t>(max_frames) * 2);
    ctx.warn("The module is longer than the render limit; it was cut.");
  }
  if (audio.samples.empty()) return fail(Status::Decode, "the module produced no sound");
  if (!m.title.empty()) audio.tags["title"] = m.title;
  return audio;
}

}  // namespace cvt::lossy::synth
