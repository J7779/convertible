// Convertible core - audio_lossy family: loaders for ProTracker MOD (and
// 15-sample Soundtracker), Scream Tracker 3 S3M, FastTracker 2 XM and
// Impulse Tracker IT (including IT214/IT215 sample compression).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossy/synth/tracker.hpp"
#include "util/text.hpp"

namespace cvt::lossy::synth {

namespace {

constexpr std::size_t max_sample_frames = 16u << 20;
constexpr std::size_t max_total_samples = 64u << 20;

std::uint16_t le16(std::span<const std::uint8_t> d, std::size_t at) {
  return at + 2 <= d.size() ? static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8)) : 0;
}
std::uint32_t le32(std::span<const std::uint8_t> d, std::size_t at) {
  return at + 4 <= d.size() ? d[at] | (std::uint32_t{d[at + 1]} << 8) | (std::uint32_t{d[at + 2]} << 16) | (std::uint32_t{d[at + 3]} << 24)
                            : 0;
}
std::uint8_t u8(std::span<const std::uint8_t> d, std::size_t at) { return at < d.size() ? d[at] : 0; }

std::string text(std::span<const std::uint8_t> d, std::size_t at, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n && at + i < d.size() && d[at + i] != 0; ++i) s.push_back(static_cast<char>(d[at + i]));
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return util::decode_text(as_bytes(s), util::TextEncoding::Windows1252).text;
}

struct Budget {
  std::size_t total = 0;
  Result<void> take(std::size_t frames) {
    if (frames > max_sample_frames) return fail(Status::Limit, "a module sample is too long");
    total += frames;
    if (total > max_total_samples) return fail(Status::Limit, "the module's samples are too large");
    return {};
  }
};

/// 8-bit or 16-bit PCM (signed/unsigned, optionally delta-coded) to floats;
/// reads what is there when the data is cut short.
std::vector<float> pcm(std::span<const std::uint8_t> d, std::size_t at, std::size_t frames, bool sixteen, bool is_signed,
                       bool delta, bool stereo = false) {
  std::vector<float> out;
  const std::size_t bytes_per = sixteen ? 2 : 1;
  const std::size_t channels = stereo ? 2 : 1;
  if (at >= d.size()) return out;
  frames = std::min(frames, (d.size() - at) / (bytes_per * channels));
  out.assign(frames, 0.0f);
  for (std::size_t c = 0; c < channels; ++c) {
    int acc = 0;
    const std::size_t base = at + c * frames * bytes_per;
    for (std::size_t i = 0; i < frames; ++i) {
      int v = sixteen ? static_cast<int>(le16(d, base + i * 2)) : static_cast<int>(u8(d, base + i));
      if (delta) {
        acc = sixteen ? static_cast<std::int16_t>(acc + v) : static_cast<std::int8_t>(acc + v);
        v = acc;
      } else if (is_signed) {
        v = sixteen ? static_cast<std::int16_t>(v) : static_cast<std::int8_t>(v);
      } else {
        v -= sixteen ? 32768 : 128;
      }
      out[i] += static_cast<float>(v / (sixteen ? 32768.0 : 128.0) / static_cast<double>(channels));
    }
  }
  return out;
}

void set_loop(Sample& s, std::size_t start, std::size_t end, int type) {
  end = std::min(end, s.data.size());
  if (type == 0 || start + 2 > end) {
    s.loop = 0;
    return;
  }
  s.loop = type;
  s.loop_start = start;
  s.loop_end = end;
}

Fx s3m_effect(char letter, std::uint8_t& param, bool it) {
  switch (letter) {
    case 'A': return Fx::SetSpeed;
    case 'B': return Fx::PosJump;
    case 'C':
      if (!it) param = static_cast<std::uint8_t>((param >> 4) * 10 + (param & 15));
      return Fx::PatBreak;
    case 'D': return Fx::VolSlideS3M;
    case 'E':
      if (param >= 0xF0) {
        param &= 15;
        return Fx::FinePortaDown;
      }
      if (param >= 0xE0) {
        param &= 15;
        return Fx::ExtraFineDown;
      }
      return Fx::PortaDown;
    case 'F':
      if (param >= 0xF0) {
        param &= 15;
        return Fx::FinePortaUp;
      }
      if (param >= 0xE0) {
        param &= 15;
        return Fx::ExtraFineUp;
      }
      return Fx::PortaUp;
    case 'G': return Fx::TonePorta;
    case 'H':
    case 'U': return Fx::Vibrato;
    case 'J': return Fx::Arpeggio;
    case 'K': return Fx::VibratoVol;
    case 'L': return Fx::TonePortaVol;
    case 'O': return Fx::Offset;
    case 'Q': return Fx::Retrig;
    case 'R': return Fx::Tremolo;
    case 'S': {
      const int sub = param >> 4, x = param & 15;
      param = static_cast<std::uint8_t>(x);
      switch (sub) {
        case 8:
          param = static_cast<std::uint8_t>(x * 17);
          return Fx::SetPan;
        case 0xB: return Fx::PatLoop;
        case 0xC: return Fx::NoteCut;
        case 0xD: return Fx::NoteDelay;
        case 0xE: return Fx::PatDelay;
        default: return Fx::None;
      }
    }
    case 'T': return param >= 0x20 ? Fx::SetTempo : Fx::None;
    case 'V':
      if (it) param = static_cast<std::uint8_t>(std::min(param, std::uint8_t{128}) / 2);
      return Fx::GlobalVolume;
    case 'W': return Fx::GlobalVolSlide;
    case 'X':
      if (!it) param = static_cast<std::uint8_t>(std::min(255, param * 2));
      return Fx::SetPan;
    default: return Fx::None;
  }
}

/// MOD/XM effect numbers 0..F, E sub-effects and XM's letters G..X.
Fx mod_effect(int effect, std::uint8_t& param) {
  switch (effect) {
    case 0x0: return param != 0 ? Fx::Arpeggio : Fx::None;
    case 0x1: return Fx::PortaUp;
    case 0x2: return Fx::PortaDown;
    case 0x3: return Fx::TonePorta;
    case 0x4: return Fx::Vibrato;
    case 0x5: return Fx::TonePortaVol;
    case 0x6: return Fx::VibratoVol;
    case 0x7: return Fx::Tremolo;
    case 0x8: return Fx::SetPan;
    case 0x9: return Fx::Offset;
    case 0xA: return Fx::VolSlide;
    case 0xB: return Fx::PosJump;
    case 0xC:
      param = std::min(param, std::uint8_t{64});
      return Fx::SetVolume;
    case 0xD: param = static_cast<std::uint8_t>((param >> 4) * 10 + (param & 15)); return Fx::PatBreak;
    case 0xE: {
      const int sub = param >> 4;
      param &= 15;
      switch (sub) {
        case 0x1: return Fx::FinePortaUp;
        case 0x2: return Fx::FinePortaDown;
        case 0x6: return Fx::PatLoop;
        case 0x8:
          param = static_cast<std::uint8_t>(param * 17);
          return Fx::SetPan;
        case 0x9: return Fx::Retrig;
        case 0xA: return Fx::FineVolUp;
        case 0xB: return Fx::FineVolDown;
        case 0xC: return Fx::NoteCut;
        case 0xD: return Fx::NoteDelay;
        case 0xE: return Fx::PatDelay;
        default: return Fx::None;
      }
    }
    case 0xF:
      if (param == 0) return Fx::None;
      return param < 32 ? Fx::SetSpeed : Fx::SetTempo;
    case 16: param = std::min(param, std::uint8_t{64}); return Fx::GlobalVolume;  // G
    case 17: return Fx::GlobalVolSlide;                                            // H
    case 20: return Fx::KeyOff;                                                    // K
    case 25: return Fx::PanSlide;                                                  // P
    case 27: return Fx::Retrig;                                                    // R
    case 33: {                                                                     // X
      const int sub = param >> 4;
      param &= 15;
      return sub == 1 ? Fx::ExtraFineUp : sub == 2 ? Fx::ExtraFineDown : Fx::None;
    }
    default: return Fx::None;
  }
}

}  // namespace

// =========================================================================== MOD

Result<Module> load_mod(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  Module m;
  m.format = "mod";
  m.clock = 14187578.4;  // PAL Amiga, period in quarter units
  if (d.size() < 600) return fail(Status::Decode, "the MOD file is too short");
  int samples = 31;
  const std::string sig = d.size() >= 1084 ? std::string(reinterpret_cast<const char*>(d.data() + 1080), 4) : std::string();
  if (sig == "M.K." || sig == "M!K!" || sig == "FLT4" || sig == "4CHN") m.channels = 4;
  else if (sig == "6CHN") m.channels = 6;
  else if (sig == "8CHN" || sig == "FLT8" || sig == "CD81" || sig == "OKTA") m.channels = 8;
  else if (sig.size() == 4 && util::is_ascii_digit(sig[0]) && util::is_ascii_digit(sig[1]) && (sig.substr(2) == "CH" || sig.substr(2) == "CN")) {
    m.channels = (sig[0] - '0') * 10 + (sig[1] - '0');
  } else if (sig.size() == 4 && util::is_ascii_digit(sig[0]) && sig.substr(1) == "CHN") {
    m.channels = sig[0] - '0';
  } else {
    samples = 15;  // Soundtracker
  }
  if (m.channels < 1 || m.channels > 32) return fail(Status::Decode, "unsupported MOD channel count");
  m.title = text(d, 0, 20);
  const std::size_t order_at = samples == 31 ? 950 : 470;
  const int length = u8(d, order_at);
  m.restart = u8(d, order_at + 1);
  if (length == 0 || length > 128) return fail(Status::Decode, "invalid MOD song length");
  int patterns = 0;
  for (int i = 0; i < 128; ++i) patterns = std::max(patterns, u8(d, order_at + 2 + static_cast<std::size_t>(i)) + 1);
  for (int i = 0; i < length; ++i) m.orders.push_back(u8(d, order_at + 2 + static_cast<std::size_t>(i)));
  if (patterns > 128) return fail(Status::Decode, "invalid MOD pattern numbers");
  const std::size_t pattern_at = order_at + 130 + (samples == 31 ? 4 : 0);
  const std::size_t pattern_size = 64 * 4 * static_cast<std::size_t>(m.channels);
  m.samples.resize(static_cast<std::size_t>(samples) + 1);
  std::vector<std::size_t> lengths(static_cast<std::size_t>(samples) + 1);
  for (int s = 1; s <= samples; ++s) {
    const std::size_t at = 20 + static_cast<std::size_t>(s - 1) * 30;
    Sample& smp = m.samples[static_cast<std::size_t>(s)];
    lengths[static_cast<std::size_t>(s)] = std::size_t{static_cast<std::uint16_t>((u8(d, at + 22) << 8) | u8(d, at + 23))} * 2;
    const int ft = static_cast<std::int8_t>(static_cast<std::uint8_t>(u8(d, at + 24) << 4)) >> 4;
    smp.c5 = 8287.1369 * std::pow(2.0, ft / 96.0);
    smp.volume = std::min(64, static_cast<int>(u8(d, at + 25)));
  }
  std::size_t sample_at = pattern_at + pattern_size * static_cast<std::size_t>(patterns);
  for (int p = 0; p < patterns; ++p) {
    Pattern pat;
    pat.rows = 64;
    pat.cells.resize(64 * static_cast<std::size_t>(m.channels));
    for (std::size_t i = 0; i < pat.cells.size(); ++i) {
      const std::size_t at = pattern_at + static_cast<std::size_t>(p) * pattern_size + i * 4;
      const std::uint8_t b0 = u8(d, at), b1 = u8(d, at + 1), b2 = u8(d, at + 2), b3 = u8(d, at + 3);
      Cell& c = pat.cells[i];
      c.instrument = static_cast<std::uint8_t>((b0 & 0xF0) | (b2 >> 4));
      if (c.instrument > samples) c.instrument = 0;
      const int period = ((b0 & 0x0F) << 8) | b1;
      if (period > 0) c.note = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(60 + 12 * std::log2(428.0 / period))), 1, 120));
      c.param = b3;
      c.fx = mod_effect(b2 & 0x0F, c.param);
    }
    m.patterns.push_back(std::move(pat));
  }
  Budget budget;
  for (int s = 1; s <= samples; ++s) {
    Sample& smp = m.samples[static_cast<std::size_t>(s)];
    const std::size_t at = 20 + static_cast<std::size_t>(s - 1) * 30;
    CVT_TRY(budget.take(lengths[static_cast<std::size_t>(s)]));
    smp.data = pcm(d, sample_at, lengths[static_cast<std::size_t>(s)], false, true, false);
    sample_at += lengths[static_cast<std::size_t>(s)];
    std::size_t loop_start = std::size_t{static_cast<std::uint16_t>((u8(d, at + 26) << 8) | u8(d, at + 27))} * 2;
    const std::size_t loop_len = std::size_t{static_cast<std::uint16_t>((u8(d, at + 28) << 8) | u8(d, at + 29))} * 2;
    if (samples == 15) loop_start /= 2;  // Soundtracker stored the start in bytes
    if (loop_len > 2) set_loop(smp, loop_start, loop_start + loop_len, 1);
  }
  m.channel_pan.resize(static_cast<std::size_t>(m.channels));
  for (int c = 0; c < m.channels; ++c) m.channel_pan[static_cast<std::size_t>(c)] = (c % 4 == 0 || c % 4 == 3) ? 64 : 192;
  m.channel_volume.assign(static_cast<std::size_t>(m.channels), 64);
  (void)ctx;
  return m;
}

// =========================================================================== S3M

Result<Module> load_s3m(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  if (d.size() < 96 || std::memcmp(d.data() + 44, "SCRM", 4) != 0) return fail(Status::Decode, "not an S3M module");
  Module m;
  m.format = "s3m";
  m.title = text(d, 0, 28);
  const int orders = le16(d, 32), instruments = le16(d, 34), patterns = le16(d, 36);
  const int ffi = le16(d, 42);
  if (orders > 256 || instruments > 255 || patterns > 256) return fail(Status::Decode, "invalid S3M header");
  m.global_volume = std::min(64, static_cast<int>(u8(d, 48)));
  m.speed = std::max(1, static_cast<int>(u8(d, 49)));
  m.tempo = std::max(32, static_cast<int>(u8(d, 50)));
  const int master = u8(d, 51);
  m.mix_gain = (master & 0x7F) / 64.0;
  if (m.mix_gain <= 0.0) m.mix_gain = 1.0;
  const bool stereo = (master & 0x80) != 0;
  std::array<int, 32> map{};
  int channels = 0;
  for (int c = 0; c < 32; ++c) {
    const int setting = u8(d, 64 + static_cast<std::size_t>(c));
    map[static_cast<std::size_t>(c)] = setting < 16 ? channels++ : -1;
    if (setting < 16) m.channel_pan.push_back(!stereo ? 128 : setting < 8 ? 64 : 192);
  }
  if (channels == 0) return fail(Status::Decode, "the S3M module has no enabled channels");
  m.channels = channels;
  m.channel_volume.assign(static_cast<std::size_t>(channels), 64);
  std::size_t pos = 96;
  for (int i = 0; i < orders; ++i) {
    const int o = u8(d, pos + static_cast<std::size_t>(i));
    if (o == 255) break;
    m.orders.push_back(o == 254 ? -2 : o);
  }
  pos += static_cast<std::size_t>(orders);
  const std::size_t ins_at = pos, pat_at = pos + static_cast<std::size_t>(instruments) * 2;
  if (u8(d, 53) == 252) {
    const std::size_t pan_at = pat_at + static_cast<std::size_t>(patterns) * 2;
    for (int c = 0; c < 32; ++c) {
      const int v = u8(d, pan_at + static_cast<std::size_t>(c));
      if (map[static_cast<std::size_t>(c)] >= 0 && (v & 0x20) != 0) m.channel_pan[static_cast<std::size_t>(map[static_cast<std::size_t>(c)])] = (v & 15) * 17;
    }
  }
  m.samples.resize(static_cast<std::size_t>(instruments) + 1);
  Budget budget;
  for (int i = 0; i < instruments; ++i) {
    const std::size_t at = std::size_t{le16(d, ins_at + static_cast<std::size_t>(i) * 2)} * 16;
    if (at + 80 > d.size() || u8(d, at) != 1) continue;
    Sample& s = m.samples[static_cast<std::size_t>(i) + 1];
    const std::size_t data_at = ((std::size_t{u8(d, at + 13)} << 16) | le16(d, at + 14)) * 16;
    const std::size_t length = le32(d, at + 16);
    const int flags = u8(d, at + 31);
    const bool sixteen = (flags & 4) != 0;
    CVT_TRY(budget.take(length));
    s.data = pcm(d, data_at, length, sixteen, ffi == 1, false, (flags & 2) != 0);
    s.volume = std::min(64, static_cast<int>(u8(d, at + 28)));
    s.c5 = le32(d, at + 32) == 0 ? 8363.0 : le32(d, at + 32);
    set_loop(s, le32(d, at + 20), le32(d, at + 24), (flags & 1) != 0 ? 1 : 0);
  }
  for (int p = 0; p < patterns; ++p) {
    Pattern pat;
    pat.rows = 64;
    pat.cells.resize(64 * static_cast<std::size_t>(channels));
    const std::size_t at = std::size_t{le16(d, pat_at + static_cast<std::size_t>(p) * 2)} * 16;
    if (at != 0 && at + 2 <= d.size()) {
      std::size_t q = at + 2;
      const std::size_t end = std::min(d.size(), at + 2 + le16(d, at));
      int row = 0;
      while (q < end && row < 64) {
        const std::uint8_t what = d[q++];
        if (what == 0) {
          ++row;
          continue;
        }
        Cell dummy;
        const int ch = map[what & 31];
        Cell& c = ch >= 0 ? pat.cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] : dummy;
        if ((what & 32) != 0) {
          const int note = u8(d, q), ins = u8(d, q + 1);
          q += 2;
          if (note == 254) c.note = 254;
          else if (note < 0xA0) c.note = static_cast<std::uint8_t>(std::clamp((note >> 4) * 12 + (note & 15) + 12, 1, 120));
          c.instrument = static_cast<std::uint8_t>(ins <= instruments ? ins : 0);
        }
        if ((what & 64) != 0) {
          c.vol_cmd = VolCmd::Set;
          c.vol = static_cast<std::uint8_t>(std::min(64, static_cast<int>(u8(d, q++))));
        }
        if ((what & 128) != 0) {
          const int cmd = u8(d, q);
          c.param = u8(d, q + 1);
          q += 2;
          if (cmd >= 1 && cmd <= 26) c.fx = s3m_effect(static_cast<char>('A' + cmd - 1), c.param, false);
        }
      }
    }
    m.patterns.push_back(std::move(pat));
  }
  (void)ctx;
  return m;
}

// =========================================================================== XM

Result<Module> load_xm(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  if (d.size() < 80 || std::memcmp(d.data(), "Extended Module: ", 17) != 0) return fail(Status::Decode, "not an XM module");
  Module m;
  m.format = "xm";
  m.instruments = true;
  m.title = text(d, 17, 20);
  const std::size_t header = le32(d, 60);
  const int length = le16(d, 64);
  m.restart = le16(d, 66);
  m.channels = le16(d, 68);
  const int patterns = le16(d, 70);
  const int instruments = le16(d, 72);
  m.linear = (le16(d, 74) & 1) != 0;
  m.speed = std::max(1, static_cast<int>(le16(d, 76)));
  m.tempo = std::max(32, static_cast<int>(le16(d, 78)));
  if (m.channels < 1 || m.channels > 64 || patterns > 256 || instruments > 255 || length > 256) {
    return fail(Status::Decode, "invalid XM header");
  }
  for (int i = 0; i < length; ++i) m.orders.push_back(u8(d, 80 + static_cast<std::size_t>(i)));
  m.channel_pan.assign(static_cast<std::size_t>(m.channels), 128);
  m.channel_volume.assign(static_cast<std::size_t>(m.channels), 64);
  std::size_t pos = 60 + header;
  for (int p = 0; p < patterns; ++p) {
    const std::size_t head = le32(d, pos);
    const int rows = le16(d, pos + 5);
    const std::size_t packed = le16(d, pos + 7);
    if (head < 9 || rows < 1 || rows > 256) return fail(Status::Decode, "invalid XM pattern header");
    Pattern pat;
    pat.rows = rows;
    pat.cells.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(m.channels));
    std::size_t q = pos + head;
    const std::size_t end = std::min(d.size(), q + packed);
    for (std::size_t i = 0; i < pat.cells.size() && q < end; ++i) {
      std::uint8_t b = d[q++];
      std::uint8_t fields[5] = {0, 0, 0, 0, 0};
      if ((b & 0x80) != 0) {
        for (int k = 0; k < 5; ++k) {
          if ((b & (1 << k)) != 0) fields[k] = u8(d, q++);
        }
      } else {
        fields[0] = b;
        for (int k = 1; k < 5; ++k) fields[k] = u8(d, q++);
      }
      Cell& c = pat.cells[i];
      if (fields[0] == 97) c.note = 255;
      else if (fields[0] >= 1 && fields[0] <= 96) c.note = static_cast<std::uint8_t>(fields[0] + 11);
      c.instrument = fields[1];
      const int v = fields[2];
      if (v >= 0x10 && v <= 0x50) {
        c.vol_cmd = VolCmd::Set;
        c.vol = static_cast<std::uint8_t>(v - 0x10);
      } else if (v >= 0x60) {
        const int x = v & 15;
        switch (v >> 4) {
          case 6: c.vol_cmd = VolCmd::SlideDown; break;
          case 7: c.vol_cmd = VolCmd::SlideUp; break;
          case 8: c.vol_cmd = VolCmd::FineDown; break;
          case 9: c.vol_cmd = VolCmd::FineUp; break;
          case 0xB: c.vol_cmd = VolCmd::Vibrato; break;
          case 0xC: c.vol_cmd = VolCmd::Pan; break;
          case 0xF: c.vol_cmd = VolCmd::TonePorta; break;
          default: break;
        }
        c.vol = static_cast<std::uint8_t>(c.vol_cmd == VolCmd::Pan ? x * 17 : c.vol_cmd == VolCmd::TonePorta ? x * 16 : x);
      }
      c.param = fields[4];
      c.fx = mod_effect(fields[3], c.param);
    }
    pos += head + packed;
    m.patterns.push_back(std::move(pat));
    if (pos > d.size()) break;
  }
  m.samples.resize(1);
  m.instrument_list.resize(static_cast<std::size_t>(instruments) + 1);
  Budget budget;
  for (int i = 1; i <= instruments && pos + 29 <= d.size(); ++i) {
    const std::size_t size = le32(d, pos);
    const int nsamples = le16(d, pos + 27);
    Instrument& ins = m.instrument_list[static_cast<std::size_t>(i)];
    if (size < 29 || nsamples > 32) return fail(Status::Decode, "invalid XM instrument");
    std::size_t sample_header = 40;
    std::array<std::uint8_t, 96> keymap{};
    if (nsamples > 0) {
      sample_header = le32(d, pos + 29);
      for (std::size_t k = 0; k < 96; ++k) keymap[k] = u8(d, pos + 33 + k);
      const auto envelope = [&](std::size_t points_at, std::size_t count_at, std::size_t sus_at, std::size_t ls_at,
                                std::size_t le_at, std::size_t type_at, Envelope& env) {
        const int count = std::min(12, static_cast<int>(u8(d, pos + count_at)));
        const int type = u8(d, pos + type_at);
        env.enabled = (type & 1) != 0 && count > 0;
        for (int p = 0; p < count; ++p) {
          env.points.emplace_back(le16(d, pos + points_at + static_cast<std::size_t>(p) * 4),
                                  std::min(64, static_cast<int>(le16(d, pos + points_at + static_cast<std::size_t>(p) * 4 + 2))));
        }
        if ((type & 2) != 0) env.sustain_start = env.sustain_end = u8(d, pos + sus_at);
        if ((type & 4) != 0) {
          env.loop_start = u8(d, pos + ls_at);
          env.loop_end = u8(d, pos + le_at);
        }
      };
      envelope(129, 225, 227, 228, 229, 233, ins.volume);
      envelope(177, 226, 230, 231, 232, 234, ins.panning);
      ins.fadeout = le16(d, pos + 239) * 2;
    }
    pos += size;
    const std::size_t first_sample = m.samples.size();
    std::vector<std::pair<std::size_t, int>> headers;  // (length bytes, type)
    for (int s = 0; s < nsamples; ++s) {
      const std::size_t at = pos + static_cast<std::size_t>(s) * sample_header;
      Sample smp;
      const int type = u8(d, at + 14);
      smp.volume = std::min(64, static_cast<int>(u8(d, at + 12)));
      smp.relative = static_cast<std::int8_t>(u8(d, at + 16)) * 64 + static_cast<std::int8_t>(u8(d, at + 13)) / 2;
      smp.pan = u8(d, at + 15);
      smp.c5 = 8363.0;
      headers.emplace_back(le32(d, at), type);
      smp.loop_start = le32(d, at + 4);
      smp.loop_end = le32(d, at + 4) + le32(d, at + 8);
      m.samples.push_back(std::move(smp));
    }
    pos += static_cast<std::size_t>(nsamples) * sample_header;
    for (int s = 0; s < nsamples; ++s) {
      Sample& smp = m.samples[first_sample + static_cast<std::size_t>(s)];
      const auto [bytes, type] = headers[static_cast<std::size_t>(s)];
      const bool sixteen = (type & 16) != 0;
      const std::size_t frames = sixteen ? bytes / 2 : bytes;
      CVT_TRY(budget.take(frames));
      smp.data = pcm(d, pos, frames, sixteen, true, true);
      const std::size_t div = sixteen ? 2 : 1;
      set_loop(smp, smp.loop_start / div, smp.loop_end / div, (type & 3) == 1 ? 1 : (type & 3) == 2 ? 2 : 0);
      pos += bytes;
    }
    for (std::size_t n = 1; n <= 120; ++n) {
      const std::size_t k = n >= 12 ? std::min<std::size_t>(n - 12, 95) : 0;
      ins.sample[n] = nsamples > 0 && keymap[k] < nsamples ? static_cast<std::uint8_t>(first_sample + keymap[k]) : 0;
      ins.note[n] = static_cast<std::uint8_t>(n);
    }
    if ((i & 15) == 0) CVT_TRY(ctx.checkpoint(0.0, "Reading"));
  }
  if (m.samples.size() > 255) return fail(Status::Decode, "too many XM samples");
  return m;
}

// =========================================================================== IT

namespace {

/// IT214/IT215 compressed sample data.
std::vector<float> it_decompress(std::span<const std::uint8_t> d, std::size_t at, std::size_t frames, bool sixteen, bool it215) {
  std::vector<float> out;
  out.reserve(frames);
  std::size_t pos = at;
  while (out.size() < frames && pos + 2 <= d.size()) {
    const std::size_t block = le16(d, pos);
    pos += 2;
    const std::size_t end = std::min(d.size(), pos + block);
    std::size_t bit = pos * 8;
    const auto read = [&](int n) -> std::uint32_t {
      std::uint32_t v = 0;
      for (int i = 0; i < n; ++i, ++bit) {
        if ((bit >> 3) >= end) return v;
        v |= static_cast<std::uint32_t>((d[bit >> 3] >> (bit & 7)) & 1) << i;
      }
      return v;
    };
    const std::size_t block_frames = std::min(frames - out.size(), sixteen ? std::size_t{0x4000} : std::size_t{0x8000});
    const int top = sixteen ? 17 : 9;
    int width = top;
    std::int32_t d1 = 0, d2 = 0;
    std::size_t done = 0;
    while (done < block_frames && (bit >> 3) < end) {
      std::uint32_t v = read(width);
      if (width < (sixteen ? 7 : 7)) {
        if (v == (1u << (width - 1))) {
          const int nw = static_cast<int>(read(sixteen ? 4 : 3)) + 1;
          width = nw < width ? nw : nw + 1;
          continue;
        }
      } else if (width < top) {
        const std::uint32_t border = (sixteen ? (0xFFFFu >> (17 - width)) : (0xFFu >> (9 - width))) - (sixteen ? 8u : 4u);
        if (v > border && v <= border + (sixteen ? 16u : 8u)) {
          const int nw = static_cast<int>(v - border);
          width = nw < width ? nw : nw + 1;
          continue;
        }
      } else if (width == top) {
        if ((v & (sixteen ? 0x10000u : 0x100u)) != 0) {
          width = static_cast<int>((v + 1) & 0xFF);
          if (width < 1 || width > top) return out;
          continue;
        }
      } else {
        return out;
      }
      std::int32_t value = 0;
      const int bits = sixteen ? 16 : 8;
      if (width < bits) {
        const int shift = 32 - width;
        value = static_cast<std::int32_t>(v << shift) >> shift;
      } else {
        value = sixteen ? static_cast<std::int16_t>(v) : static_cast<std::int8_t>(v);
      }
      d1 = sixteen ? static_cast<std::int16_t>(d1 + value) : static_cast<std::int8_t>(d1 + value);
      d2 = sixteen ? static_cast<std::int16_t>(d2 + d1) : static_cast<std::int8_t>(d2 + d1);
      out.push_back(static_cast<float>((it215 ? d2 : d1) / (sixteen ? 32768.0 : 128.0)));
      ++done;
    }
    pos = end;
  }
  return out;
}

}  // namespace

Result<Module> load_it(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  if (d.size() < 192 || std::memcmp(d.data(), "IMPM", 4) != 0) return fail(Status::Decode, "not an IT module");
  Module m;
  m.format = "it";
  m.title = text(d, 4, 26);
  const int orders = le16(d, 32), instruments = le16(d, 34), samples = le16(d, 36), patterns = le16(d, 38);
  const int cmwt = le16(d, 42), flags = le16(d, 44);
  if (orders > 256 || instruments > 255 || samples > 255 || patterns > 256) return fail(Status::Decode, "invalid IT header");
  m.instruments = (flags & 4) != 0;
  m.linear = (flags & 8) != 0;
  m.global_volume = std::min(64, static_cast<int>(u8(d, 48)) / 2);
  m.mix_gain = std::min(128, static_cast<int>(u8(d, 49))) / 48.0;
  m.speed = std::max(1, static_cast<int>(u8(d, 50)));
  m.tempo = std::max(32, static_cast<int>(u8(d, 51)));
  m.channels = 64;
  std::vector<int> pans(64), vols(64);
  for (int c = 0; c < 64; ++c) {
    const int p = u8(d, 64 + static_cast<std::size_t>(c));
    pans[static_cast<std::size_t>(c)] = (p & 0x7F) > 64 ? 128 : (p & 0x7F) * 255 / 64;
    vols[static_cast<std::size_t>(c)] = std::min(64, static_cast<int>(u8(d, 128 + static_cast<std::size_t>(c))));
  }
  std::size_t pos = 192;
  for (int i = 0; i < orders; ++i) {
    const int o = u8(d, pos + static_cast<std::size_t>(i));
    if (o == 255) break;
    m.orders.push_back(o == 254 ? -2 : o);
  }
  pos += static_cast<std::size_t>(orders);
  const std::size_t ins_at = pos, smp_at = pos + static_cast<std::size_t>(instruments) * 4,
                    pat_at = smp_at + static_cast<std::size_t>(samples) * 4;
  // Samples.
  m.samples.resize(static_cast<std::size_t>(samples) + 1);
  Budget budget;
  for (int s = 0; s < samples; ++s) {
    const std::size_t at = le32(d, smp_at + static_cast<std::size_t>(s) * 4);
    if (at + 80 > d.size() || std::memcmp(d.data() + at, "IMPS", 4) != 0) continue;
    Sample& smp = m.samples[static_cast<std::size_t>(s) + 1];
    const int sflags = u8(d, at + 18);
    smp.global = std::min(64, static_cast<int>(u8(d, at + 17)));
    smp.volume = std::min(64, static_cast<int>(u8(d, at + 19)));
    const int cvt = u8(d, at + 46);
    const int dfp = u8(d, at + 47);
    if ((dfp & 0x80) != 0) smp.pan = std::min(64, dfp & 0x7F) * 255 / 64;
    const std::size_t length = le32(d, at + 48);
    smp.c5 = std::max<std::uint32_t>(1, le32(d, at + 60));
    const std::size_t data_at = le32(d, at + 72);
    if ((sflags & 1) == 0) continue;
    CVT_TRY(budget.take(length));
    const bool sixteen = (sflags & 2) != 0;
    if ((sflags & 8) != 0) smp.data = it_decompress(d, data_at, length, sixteen, (cvt & 4) != 0);
    else smp.data = pcm(d, data_at, length, sixteen, (cvt & 1) != 0, false, (sflags & 4) != 0);
    set_loop(smp, le32(d, at + 52), le32(d, at + 56), (sflags & 16) == 0 ? 0 : (sflags & 64) != 0 ? 2 : 1);
  }
  // Instruments (new format only; old ones play their samples directly).
  m.instrument_list.resize(static_cast<std::size_t>(instruments) + 1);
  for (int i = 0; i < instruments; ++i) {
    const std::size_t at = le32(d, ins_at + static_cast<std::size_t>(i) * 4);
    Instrument& ins = m.instrument_list[static_cast<std::size_t>(i) + 1];
    if (at + 554 > d.size() || std::memcmp(d.data() + at, "IMPI", 4) != 0 || cmwt < 0x200) continue;
    ins.fadeout = le16(d, at + 20) * 32;
    ins.global = std::min(128, static_cast<int>(u8(d, at + 24))) / 2;
    // Notes are numbered as in IT (60 = C-5 = middle C); note 0 (C-0) is
    // folded onto 1 because 0 means "no note" here.
    for (std::size_t n = 1; n < 120; ++n) {
      ins.note[n] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(u8(d, at + 64 + n * 2)), 1, 119));
      const int smp = u8(d, at + 65 + n * 2);
      ins.sample[n] = static_cast<std::uint8_t>(smp <= samples ? smp : 0);
    }
    const std::size_t env = at + 304;
    const int ef = u8(d, env);
    const int count = std::min(25, static_cast<int>(u8(d, env + 1)));
    ins.volume.enabled = (ef & 1) != 0 && count > 0;
    for (int p = 0; p < count; ++p) {
      ins.volume.points.emplace_back(le16(d, env + 7 + static_cast<std::size_t>(p) * 3),
                                     std::min(64, static_cast<int>(u8(d, env + 6 + static_cast<std::size_t>(p) * 3))));
    }
    if ((ef & 2) != 0) {
      ins.volume.loop_start = u8(d, env + 2);
      ins.volume.loop_end = u8(d, env + 3);
    }
    if ((ef & 4) != 0) {
      ins.volume.sustain_start = u8(d, env + 4);
      ins.volume.sustain_end = u8(d, env + 5);
    }
  }
  // Patterns.
  int used_channels = 1;
  for (int p = 0; p < patterns; ++p) {
    const std::size_t at = le32(d, pat_at + static_cast<std::size_t>(p) * 4);
    Pattern pat;
    pat.rows = 64;
    if (at != 0 && at + 8 <= d.size()) pat.rows = std::clamp(static_cast<int>(le16(d, at + 2)), 1, 200);
    pat.cells.resize(static_cast<std::size_t>(pat.rows) * 64);
    if (at != 0 && at + 8 <= d.size()) {
      std::size_t q = at + 8;
      const std::size_t end = std::min(d.size(), q + le16(d, at));
      std::array<std::uint8_t, 64> last_mask{};
      std::array<Cell, 64> last{};
      std::array<std::uint8_t, 64> last_cmd{}, last_param{};
      int row = 0;
      while (q < end && row < pat.rows) {
        const std::uint8_t var = d[q++];
        if (var == 0) {
          ++row;
          continue;
        }
        const std::size_t ch = static_cast<std::size_t>((var - 1) & 63);
        used_channels = std::max(used_channels, static_cast<int>(ch) + 1);
        if ((var & 0x80) != 0) last_mask[ch] = u8(d, q++);
        const std::uint8_t mask = last_mask[ch];
        Cell& c = pat.cells[static_cast<std::size_t>(row) * 64 + ch];
        if ((mask & 1) != 0) {
          const int n = u8(d, q++);
          last[ch].note = static_cast<std::uint8_t>(n < 120 ? std::max(n, 1) : n == 255 ? 255 : n == 254 ? 254 : 253);
        }
        if ((mask & 2) != 0) last[ch].instrument = u8(d, q++);
        if ((mask & 4) != 0) {
          const int v = u8(d, q++);
          Cell vc;
          if (v <= 64) {
            vc.vol_cmd = VolCmd::Set;
            vc.vol = static_cast<std::uint8_t>(v);
          } else if (v <= 74) {
            vc.vol_cmd = VolCmd::FineUp;
            vc.vol = static_cast<std::uint8_t>(v - 65);
          } else if (v <= 84) {
            vc.vol_cmd = VolCmd::FineDown;
            vc.vol = static_cast<std::uint8_t>(v - 75);
          } else if (v <= 94) {
            vc.vol_cmd = VolCmd::SlideUp;
            vc.vol = static_cast<std::uint8_t>(v - 85);
          } else if (v <= 104) {
            vc.vol_cmd = VolCmd::SlideDown;
            vc.vol = static_cast<std::uint8_t>(v - 95);
          } else if (v >= 128 && v <= 192) {
            vc.vol_cmd = VolCmd::Pan;
            vc.vol = static_cast<std::uint8_t>((v - 128) * 255 / 64);
          } else if (v >= 193 && v <= 202) {
            vc.vol_cmd = VolCmd::TonePorta;
            static constexpr std::uint8_t speeds[10] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255};
            vc.vol = speeds[v - 193];
          }
          last[ch].vol_cmd = vc.vol_cmd;
          last[ch].vol = vc.vol;
        }
        if ((mask & 8) != 0) {
          last_cmd[ch] = u8(d, q);
          last_param[ch] = u8(d, q + 1);
          q += 2;
        }
        if ((mask & (1 | 16)) != 0) c.note = last[ch].note;
        if ((mask & (2 | 32)) != 0) c.instrument = last[ch].instrument;
        if ((mask & (4 | 64)) != 0) {
          c.vol_cmd = last[ch].vol_cmd;
          c.vol = last[ch].vol;
        }
        if ((mask & (8 | 128)) != 0) {
          c.param = last_param[ch];
          const int cmd = last_cmd[ch];
          if (cmd >= 1 && cmd <= 26) c.fx = s3m_effect(static_cast<char>('A' + cmd - 1), c.param, true);
        }
      }
    }
    m.patterns.push_back(std::move(pat));
    if ((p & 15) == 0) CVT_TRY(ctx.checkpoint(0.0, "Reading"));
  }
  // Keep only the channels in use.
  m.channels = used_channels;
  for (Pattern& pat : m.patterns) {
    std::vector<Cell> cells(static_cast<std::size_t>(pat.rows) * static_cast<std::size_t>(used_channels));
    for (int r = 0; r < pat.rows; ++r) {
      for (int c = 0; c < used_channels; ++c) {
        cells[static_cast<std::size_t>(r * used_channels + c)] = pat.cells[static_cast<std::size_t>(r) * 64 + static_cast<std::size_t>(c)];
      }
    }
    pat.cells = std::move(cells);
  }
  m.channel_pan.assign(pans.begin(), pans.begin() + used_channels);
  m.channel_volume.assign(vols.begin(), vols.begin() + used_channels);
  return m;
}

}  // namespace cvt::lossy::synth
