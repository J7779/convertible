// Convertible core - audio_lossy family: Standard MIDI Files (types 0, 1 and
// 2; RIFF RMID; .kar) and a compact built-in General MIDI synthesiser:
// band-limited oscillators, two-operator FM, plucked strings, ADSR
// envelopes, synthesised drum kit, pitch bend with RPN range, volume,
// expression, pan and sustain. No sample data is bundled.
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
#include "util/text.hpp"

namespace cvt::lossy::synth {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::size_t max_events = 4'000'000;

std::uint32_t be32(std::span<const std::uint8_t> d, std::size_t at) {
  return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) | (std::uint32_t{d[at + 2]} << 8) | d[at + 3];
}

/// The SMF inside a RIFF RMID wrapper, or the data itself.
ByteView unwrap_rmid(ByteView data) {
  const auto d = as_u8(data);
  if (d.size() >= 20 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "RMID", 4) == 0) {
    std::size_t pos = 12;
    while (pos + 8 <= d.size()) {
      const std::size_t size = d[pos + 4] | (std::size_t{d[pos + 5]} << 8) | (std::size_t{d[pos + 6]} << 16) |
                               (std::size_t{d[pos + 7]} << 24);
      if (std::memcmp(d.data() + pos, "data", 4) == 0) return data.subspan(pos + 8, std::min(size, d.size() - pos - 8));
      pos += 8 + size + (size & 1);
    }
  }
  return data;
}

}  // namespace

bool looks_like_midi(ByteView data) {
  const ByteView smf = unwrap_rmid(data);
  return smf.size() >= 14 && std::memcmp(smf.data(), "MThd", 4) == 0;
}

Result<MidiFile> parse_midi(ByteView input, const Context& ctx) {
  const ByteView data = unwrap_rmid(input);
  const auto d = as_u8(data);
  if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4) != 0) return fail(Status::Decode, "not a Standard MIDI File");
  const std::uint32_t header = be32(d, 4);
  if (header < 6 || 8 + std::uint64_t{header} > d.size()) return fail(Status::Decode, "invalid MIDI header");
  MidiFile midi;
  midi.format = (d[8] << 8) | d[9];
  const int declared = (d[10] << 8) | d[11];
  midi.division = static_cast<std::int16_t>((d[12] << 8) | d[13]);
  if (midi.format > 2) return fail(Status::Decode, "unknown MIDI file type {}", midi.format);
  if (midi.division == 0) return fail(Status::Decode, "invalid MIDI time division");
  std::size_t pos = 8 + header;
  int track = 0;
  std::uint64_t type2_offset = 0;  // type 2: tracks play one after another
  while (pos + 8 <= d.size() && track < 65535) {
    const std::uint32_t size = be32(d, pos + 4);
    const bool is_track = std::memcmp(d.data() + pos, "MTrk", 4) == 0;
    const std::size_t start = pos + 8;
    const std::size_t end = static_cast<std::size_t>(std::min<std::uint64_t>(d.size(), std::uint64_t{start} + size));
    pos = end;
    if (!is_track) continue;
    std::size_t p = start;
    std::uint64_t tick = type2_offset;
    std::uint8_t running = 0;
    const auto varlen = [&](std::uint32_t& v) {
      v = 0;
      for (int i = 0; i < 4; ++i) {
        if (p >= end) return false;
        const std::uint8_t b = d[p++];
        v = (v << 7) | (b & 0x7Fu);
        if ((b & 0x80) == 0) return true;
      }
      return false;
    };
    while (p < end) {
      std::uint32_t delta = 0;
      if (!varlen(delta)) break;
      tick += delta;
      if (p >= end) break;
      std::uint8_t status = d[p];
      MidiEvent e;
      e.tick = tick;
      e.track = track;
      if (status >= 0x80) {
        ++p;
        if (status < 0xF0) running = status;
      } else {
        if (running == 0) break;  // data byte without status: damaged track
        status = running;
      }
      e.status = status;
      if (status == 0xFF) {
        if (p >= end) break;
        e.meta = d[p++];
        std::uint32_t len = 0;
        if (!varlen(len) || len > end - p) break;
        e.data.assign(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + len));
        p += len;
        if (e.meta == 0x2F) {
          midi.events.push_back(std::move(e));
          break;  // end of track
        }
      } else if (status == 0xF0 || status == 0xF7) {
        std::uint32_t len = 0;
        if (!varlen(len) || len > end - p) break;
        e.data.assign(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + len));
        p += len;
        running = 0;
      } else if (status >= 0xF1) {
        // System common/real-time bytes do not belong in files: skip one.
        continue;
      } else {
        const int kind = status & 0xF0;
        const std::size_t n = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (end - p < n) break;
        e.data.assign(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + n));
        for (auto& b : e.data) b &= 0x7F;
        p += n;
      }
      midi.events.push_back(std::move(e));
      if (midi.events.size() > max_events) return fail(Status::Limit, "the MIDI file has too many events");
    }
    if (midi.format == 2) type2_offset = tick;
    ++track;
    if ((track & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size()), "Reading"));
  }
  midi.tracks = track;
  if (track == 0) return fail(Status::Decode, "the MIDI file has no tracks");
  if (declared != track) ctx.warn("The MIDI header declares a different number of tracks than the file holds.");
  std::stable_sort(midi.events.begin(), midi.events.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
  // Tempo map -> seconds.
  double seconds = 0.0;
  std::uint64_t last_tick = 0;
  double tempo = 500000.0;  // microseconds per quarter note
  const bool smpte = midi.division < 0;
  const double ticks_per_second_smpte =
      smpte ? static_cast<double>(-(midi.division >> 8)) * static_cast<double>(midi.division & 0xFF) : 0.0;
  for (MidiEvent& e : midi.events) {
    const auto delta = static_cast<double>(e.tick - last_tick);
    seconds += smpte ? (ticks_per_second_smpte > 0 ? delta / ticks_per_second_smpte : 0.0)
                     : delta * tempo / 1e6 / static_cast<double>(midi.division);
    last_tick = e.tick;
    e.seconds = seconds;
    if (e.status == 0xFF && e.meta == 0x51 && e.data.size() == 3) {
      const double t = (e.data[0] << 16) | (e.data[1] << 8) | e.data[2];
      if (t > 0) tempo = t;
    }
    if (e.status == 0xFF && e.track == 0 && (e.meta == 0x03 || e.meta == 0x02)) {
      std::string text(e.data.begin(), e.data.end());
      text = util::decode_text(as_bytes(text), util::TextEncoding::Windows1252).text;
      if (e.meta == 0x03 && midi.title.empty()) midi.title = text;
      if (e.meta == 0x02 && midi.copyright.empty()) midi.copyright = text;
    }
  }
  midi.duration = seconds;
  return midi;
}

// =========================================================================== synthesiser

namespace {

/// Deterministic noise.
struct Lcg {
  std::uint32_t state = 22222;
  double next() {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(state >> 8) / 8388608.0 - 1.0;
  }
};

double poly_blep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return t * t + t + t + 1.0;
  }
  return 0.0;
}

enum class Timbre { Piano, Bell, Organ, Pluck, Bass, Strings, Brass, Reed, Pipe, Lead, Pad, Drum };

Timbre timbre_for(int program) {
  switch (program / 8) {
    case 0: return Timbre::Piano;
    case 1: return Timbre::Bell;
    case 2: return Timbre::Organ;
    case 3: return Timbre::Pluck;
    case 4: return Timbre::Bass;
    case 5:
    case 6: return Timbre::Strings;
    case 7: return Timbre::Brass;
    case 8:
    case 9: return Timbre::Reed;
    case 10: return Timbre::Pipe;
    case 11: return Timbre::Lead;
    case 12:
    case 13: return Timbre::Pad;
    case 14: return Timbre::Pluck;
    default: return Timbre::Bell;
  }
}

struct Envelope {
  double attack, decay, sustain, release;  // seconds, seconds, level, seconds
};

Envelope envelope_for(Timbre t) {
  switch (t) {
    case Timbre::Piano: return {0.002, 1.8, 0.0, 0.25};
    case Timbre::Bell: return {0.001, 1.2, 0.0, 0.4};
    case Timbre::Organ: return {0.01, 0.05, 0.9, 0.05};
    case Timbre::Pluck: return {0.001, 1.0, 0.0, 0.15};
    case Timbre::Bass: return {0.004, 0.6, 0.5, 0.08};
    case Timbre::Strings: return {0.12, 0.3, 0.85, 0.3};
    case Timbre::Brass: return {0.04, 0.2, 0.75, 0.12};
    case Timbre::Reed: return {0.03, 0.1, 0.85, 0.08};
    case Timbre::Pipe: return {0.05, 0.1, 0.8, 0.1};
    case Timbre::Lead: return {0.005, 0.1, 0.8, 0.08};
    case Timbre::Pad: return {0.4, 0.5, 0.8, 0.6};
    case Timbre::Drum: return {0.0005, 0.3, 0.0, 0.05};
  }
  return {0.01, 0.3, 0.7, 0.2};
}

struct Voice {
  bool active = false;
  bool released = false;
  bool sustained = false;  // note off arrived while the sustain pedal was down
  int channel = 0;
  int note = 0;
  double velocity = 0.0;
  Timbre timbre = Timbre::Piano;
  int drum = 0;
  double phase = 0.0;
  double phase2 = 0.0;
  double env = 0.0;
  double time = 0.0;
  double release_level = 0.0;
  double release_time = 0.0;
  double lp = 0.0;
  std::uint64_t age = 0;
  std::vector<double> string;  // Karplus-Strong delay line
  std::size_t string_pos = 0;
  Envelope adsr{};
};

struct Channel {
  int program = 0;
  double volume = 100.0 / 127.0;
  double expression = 1.0;
  double pan = 0.5;
  bool sustain = false;
  double bend = 0.0;          // semitones
  double bend_range = 2.0;
  int rpn = 0x3FFF;
  double mod = 0.0;
};

class Synth {
 public:
  explicit Synth(double rate) : rate_(rate) {}

  void event(const MidiEvent& e) {
    if (e.status < 0x80 || e.status >= 0xF0) return;
    const int ch = e.status & 0x0F;
    Channel& c = channels_[static_cast<std::size_t>(ch)];
    const int a = e.data.empty() ? 0 : e.data[0];
    const int b = e.data.size() > 1 ? e.data[1] : 0;
    switch (e.status & 0xF0) {
      case 0x90:
        if (b > 0) {
          note_on(ch, a, b);
          break;
        }
        [[fallthrough]];
      case 0x80:
        for (Voice& v : voices_) {
          if (v.active && !v.released && v.channel == ch && v.note == a) {
            if (c.sustain) v.sustained = true;
            else release(v);
          }
        }
        break;
      case 0xB0: controller(c, ch, a, b); break;
      case 0xC0: c.program = a; break;
      case 0xE0: c.bend = ((b << 7 | a) - 8192) / 8192.0 * c.bend_range; break;
      default: break;
    }
  }

  void render(float* out, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i) {
      double l = 0.0, r = 0.0;
      for (Voice& v : voices_) {
        if (!v.active) continue;
        const Channel& c = channels_[static_cast<std::size_t>(v.channel)];
        const double s = sample(v, c);
        const double gain = c.volume * c.volume * c.expression * 0.25;
        const double pan = v.timbre == Timbre::Drum ? 0.5 : c.pan;
        l += s * gain * std::cos(pan * std::numbers::pi / 2);
        r += s * gain * std::sin(pan * std::numbers::pi / 2);
      }
      out[2 * i] = static_cast<float>(std::tanh(l));
      out[2 * i + 1] = static_cast<float>(std::tanh(r));
    }
  }

 private:
  void controller(Channel& c, int ch, int cc, int value) {
    switch (cc) {
      case 1: c.mod = value / 127.0; break;
      case 6:
        if (c.rpn == 0) c.bend_range = value;
        break;
      case 7: c.volume = value / 127.0; break;
      case 10: c.pan = value / 127.0; break;
      case 11: c.expression = value / 127.0; break;
      case 38:
        if (c.rpn == 0) c.bend_range = std::floor(c.bend_range) + value / 100.0;
        break;
      case 64:
        c.sustain = value >= 64;
        if (!c.sustain) {
          for (Voice& v : voices_) {
            if (v.active && v.channel == ch && v.sustained) release(v);
          }
        }
        break;
      case 100: c.rpn = (c.rpn & 0x3F80) | value; break;
      case 101: c.rpn = (c.rpn & 0x7F) | (value << 7); break;
      case 120:
      case 123:
        for (Voice& v : voices_) {
          if (v.active && v.channel == ch) {
            if (cc == 120) v.active = false;
            else release(v);
          }
        }
        break;
      case 121: {
        const int program = c.program;
        c = Channel{};
        c.program = program;
        break;
      }
      default: break;
    }
  }

  void note_on(int ch, int note, int velocity) {
    // Re-striking a held note restarts it.
    Voice* slot = nullptr;
    for (Voice& v : voices_) {
      if (!v.active) {
        slot = &v;
        break;
      }
    }
    if (slot == nullptr) {
      // Steal the oldest released voice, else the oldest one.
      for (Voice& v : voices_) {
        if (slot == nullptr || (v.released && !slot->released) || (v.released == slot->released && v.age < slot->age)) slot = &v;
      }
    }
    Voice& v = *slot;
    v = Voice{};
    v.active = true;
    v.channel = ch;
    v.note = note;
    v.velocity = velocity / 127.0;
    v.age = ++counter_;
    const Channel& c = channels_[static_cast<std::size_t>(ch)];
    v.timbre = ch == 9 ? Timbre::Drum : timbre_for(c.program);
    v.drum = note;
    v.adsr = envelope_for(v.timbre);
    if (v.timbre == Timbre::Drum) {
      const bool cymbal = note == 49 || note == 51 || note == 52 || note == 55 || note == 57 || note == 59;
      const bool open_hat = note == 46;
      v.adsr.decay = cymbal ? 1.2 : open_hat ? 0.35 : (note == 42 || note == 44) ? 0.06 : note <= 36 ? 0.35 : 0.2;
    }
    if (v.timbre == Timbre::Pluck) {
      const double f = frequency(v, c);
      const auto n = static_cast<std::size_t>(std::clamp(rate_ / std::max(f, 20.0), 2.0, 4096.0));
      v.string.resize(n);
      for (double& s : v.string) s = noise_.next();
    }
  }

  void release(Voice& v) {
    if (v.released) return;
    v.released = true;
    v.release_level = v.env;
    v.release_time = 0.0;
  }

  double frequency(const Voice& v, const Channel& c) const {
    const double vib = c.mod > 0 ? c.mod * 0.3 * std::sin(two_pi * 5.5 * v.time) : 0.0;
    return 440.0 * std::pow(2.0, (v.note - 69 + c.bend + vib) / 12.0);
  }

  double sample(Voice& v, const Channel& c) {
    const double dt = 1.0 / rate_;
    // Envelope.
    const Envelope& e = v.adsr;
    if (!v.released) {
      if (v.time < e.attack) v.env = v.time / e.attack;
      else v.env = e.sustain + (1.0 - e.sustain) * std::exp(-(v.time - e.attack) / std::max(e.decay, 1e-3) * 3.0);
    } else {
      v.env = v.release_level * std::exp(-v.release_time / std::max(e.release, 1e-3) * 4.0);
      v.release_time += dt;
      if (v.release_time > e.release * 2.0) v.active = false;
    }
    if (!v.released && e.sustain == 0.0 && v.time > e.attack + e.decay * 2.5) v.active = false;
    v.time += dt;
    const double amp = v.env * (0.3 + 0.7 * v.velocity);
    if (v.timbre == Timbre::Drum) return amp * drum(v);
    const double f = frequency(v, c);
    const double inc = f / rate_;
    double s = 0.0;
    switch (v.timbre) {
      case Timbre::Piano: {
        // Additive partials with faster decay for the upper ones.
        for (int k = 1; k <= 5; ++k) {
          if (f * k > rate_ * 0.45) break;
          s += std::sin(two_pi * v.phase * k) * std::exp(-v.time * k * 1.2) / k;
        }
        s *= 0.8;
        break;
      }
      case Timbre::Bell: {
        // Two-operator FM, ratio 3.5.
        const double index = 3.0 * std::exp(-v.time * 3.0);
        s = std::sin(two_pi * v.phase + index * std::sin(two_pi * v.phase2));
        v.phase2 += inc * 3.5;
        v.phase2 -= std::floor(v.phase2);
        break;
      }
      case Timbre::Organ:
        for (const int k : {1, 2, 3, 4, 6, 8}) {
          if (f * k < rate_ * 0.45) s += std::sin(two_pi * v.phase * k) / (k == 1 ? 1.0 : 2.0 + k * 0.2);
        }
        s *= 0.5;
        break;
      case Timbre::Pluck: {
        const std::size_t n = v.string.size();
        const std::size_t next = (v.string_pos + 1) % n;
        s = v.string[v.string_pos];
        v.string[v.string_pos] = 0.498 * (v.string[v.string_pos] + v.string[next]);
        v.string_pos = next;
        break;
      }
      case Timbre::Bass:
        s = std::sin(two_pi * v.phase) + 0.3 * (2.0 * v.phase - 1.0 - poly_blep(v.phase, inc));
        break;
      case Timbre::Strings:
      case Timbre::Brass:
      case Timbre::Lead:
      case Timbre::Pad: {
        double saw = 2.0 * v.phase - 1.0 - poly_blep(v.phase, inc);
        if (v.timbre == Timbre::Pad) {
          const double p2 = v.phase2;
          saw = 0.5 * (saw + 2.0 * p2 - 1.0 - poly_blep(p2, inc * 1.006));
          v.phase2 += inc * 1.006;
          v.phase2 -= std::floor(v.phase2);
        }
        // One-pole lowpass: brighter for brass and leads.
        const double cutoff = v.timbre == Timbre::Brass ? 0.25 + 0.5 * v.env : v.timbre == Timbre::Lead ? 0.6 : 0.2;
        v.lp += cutoff * (saw - v.lp);
        s = v.lp * 0.8;
        break;
      }
      case Timbre::Reed: {
        const double sq = (v.phase < 0.5 ? 1.0 : -1.0) + poly_blep(v.phase, inc) - poly_blep(std::fmod(v.phase + 0.5, 1.0), inc);
        v.lp += 0.3 * (sq - v.lp);
        s = v.lp * 0.6;
        break;
      }
      case Timbre::Pipe:
        s = std::sin(two_pi * v.phase) + 0.05 * noise_.next();
        break;
      case Timbre::Drum: break;
    }
    v.phase += inc;
    v.phase -= std::floor(v.phase);
    return amp * s;
  }

  double drum(Voice& v) {
    const int n = v.drum;
    const double t = v.time;
    if (n == 35 || n == 36) {  // kick: falling sine
      const double f = 50.0 + 120.0 * std::exp(-t * 25.0);
      v.phase += f / rate_;
      return std::sin(two_pi * v.phase) * 1.2;
    }
    if (n == 38 || n == 40 || n == 37 || n == 39) {  // snare / clap / stick
      const double f = 190.0;
      v.phase += f / rate_;
      const double tone = std::sin(two_pi * v.phase) * std::exp(-t * 30.0);
      v.lp += 0.6 * (noise_.next() - v.lp);
      return 0.5 * tone + 0.7 * (noise_.next() - v.lp);
    }
    if (n == 41 || n == 43 || n == 45 || n == 47 || n == 48 || n == 50) {  // toms
      const double base = 80.0 + (n - 41) * 18.0;
      const double f = base * (1.0 + 0.5 * std::exp(-t * 20.0));
      v.phase += f / rate_;
      return std::sin(two_pi * v.phase);
    }
    // Hats, cymbals and everything else: high-passed noise.
    const double x = noise_.next();
    const double hp = x - v.lp;
    v.lp += 0.2 * (x - v.lp);
    return hp * 0.5;
  }

  double rate_;
  std::array<Channel, 16> channels_{};
  std::array<Voice, 48> voices_{};
  std::uint64_t counter_ = 0;
  Lcg noise_;
};

}  // namespace

Result<ir::Audio> render_midi(const MidiFile& midi, const RenderSettings& settings, const Context& ctx) {
  const double rate = settings.sample_rate;
  double length = std::min(midi.duration + 2.0, settings.max_seconds);  // two seconds of release tail
  if (midi.duration > settings.max_seconds) ctx.warn("The MIDI song is longer than the render limit; it was cut.");
  const auto frames = static_cast<std::uint64_t>(length * rate);
  CVT_TRY(audio::check_sample_budget(ctx, frames * 2));
  ir::Audio audio;
  audio.sample_rate = settings.sample_rate;
  audio.channels = 2;
  audio.samples.resize(static_cast<std::size_t>(frames * 2));
  Synth synth(rate);
  std::uint64_t done = 0;
  std::size_t next = 0;
  while (done < frames) {
    while (next < midi.events.size() && static_cast<std::uint64_t>(midi.events[next].seconds * rate) <= done) {
      synth.event(midi.events[next++]);
    }
    std::uint64_t until = frames;
    if (next < midi.events.size()) until = std::min(until, static_cast<std::uint64_t>(midi.events[next].seconds * rate));
    until = std::min(until, done + 4096);
    if (until <= done) until = done + 1;
    synth.render(audio.samples.data() + done * 2, static_cast<std::size_t>(until - done));
    done = until;
    if ((done & 0xFFFF) < 4096) CVT_TRY(ctx.checkpoint(static_cast<double>(done) / static_cast<double>(frames), "Rendering"));
  }
  if (!midi.title.empty()) audio.tags["title"] = util::sanitize_utf8(midi.title);
  if (!midi.copyright.empty()) audio.tags["copyright"] = util::sanitize_utf8(midi.copyright);
  return audio;
}

}  // namespace cvt::lossy::synth
