// Convertible core - audio_lossless family: PCM, G.711 and ADPCM sample codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/pcm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace cvt::audio {

// =========================================================================== PCM layouts

bool PcmFormat::valid() const noexcept {
  switch (encoding) {
    case PcmEncoding::Alaw:
    case PcmEncoding::Mulaw: return container_bytes == 1 && bits == 8;
    case PcmEncoding::Float: return (container_bytes == 4 && bits == 32) || (container_bytes == 8 && bits == 64);
    case PcmEncoding::Unsigned:
    case PcmEncoding::Signed:
      return container_bytes >= 1 && container_bytes <= 4 && bits >= 1 && bits <= container_bytes * 8;
  }
  return false;
}

std::string PcmFormat::name() const {
  const char* endian = order == std::endian::little ? "le" : "be";
  switch (encoding) {
    case PcmEncoding::Alaw: return "alaw";
    case PcmEncoding::Mulaw: return "mulaw";
    case PcmEncoding::Float: return std::format("f{}{}", bits, endian);
    case PcmEncoding::Unsigned:
      return container_bytes == 1 ? std::format("u{}", bits) : std::format("u{}{}", bits, endian);
    case PcmEncoding::Signed:
      return container_bytes == 1 ? std::format("s{}", bits) : std::format("s{}{}", bits, endian);
  }
  return "pcm";
}

PcmFormat pcm_signed(int bits, std::endian order) {
  PcmFormat f;
  f.encoding = PcmEncoding::Signed;
  f.bits = static_cast<std::uint16_t>(bits);
  f.container_bytes = static_cast<std::uint16_t>((bits + 7) / 8);
  f.order = order;
  return f;
}
PcmFormat pcm_unsigned8() {
  PcmFormat f;
  f.encoding = PcmEncoding::Unsigned;
  f.bits = 8;
  f.container_bytes = 1;
  return f;
}
PcmFormat pcm_float(int bits, std::endian order) {
  PcmFormat f;
  f.encoding = PcmEncoding::Float;
  f.bits = static_cast<std::uint16_t>(bits);
  f.container_bytes = static_cast<std::uint16_t>(bits / 8);
  f.order = order;
  return f;
}
PcmFormat pcm_alaw() {
  PcmFormat f;
  f.encoding = PcmEncoding::Alaw;
  f.bits = 8;
  f.container_bytes = 1;
  return f;
}
PcmFormat pcm_mulaw() {
  PcmFormat f = pcm_alaw();
  f.encoding = PcmEncoding::Mulaw;
  return f;
}

const std::vector<std::string>& sample_format_choices() {
  static const std::vector<std::string> choices = {"auto", "u8",  "s8",  "s16",  "s24",
                                                   "s32",  "f32", "f64", "alaw", "mulaw"};
  return choices;
}

ParsedSampleFormat parse_sample_format(std::string_view name) {
  ParsedSampleFormat p;
  p.ok = true;
  if (name == "u8") {
    p.encoding = PcmEncoding::Unsigned;
    p.bits = 8;
  } else if (name == "s8") {
    p.bits = 8;
  } else if (name == "s16") {
    p.bits = 16;
  } else if (name == "s24") {
    p.bits = 24;
  } else if (name == "s32") {
    p.bits = 32;
  } else if (name == "f32") {
    p.encoding = PcmEncoding::Float;
    p.bits = 32;
  } else if (name == "f64") {
    p.encoding = PcmEncoding::Float;
    p.bits = 64;
  } else if (name == "alaw") {
    p.encoding = PcmEncoding::Alaw;
    p.bits = 8;
  } else if (name == "mulaw") {
    p.encoding = PcmEncoding::Mulaw;
    p.bits = 8;
  } else {
    p.ok = false;
  }
  return p;
}

namespace {

template <std::size_t N, bool Little>
std::uint64_t load(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < N; ++i) {
    if constexpr (Little) {
      v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    } else {
      v = (v << 8) | p[i];
    }
  }
  return v;
}

template <std::size_t N, bool Little>
void decode_signed(std::span<const std::uint8_t> bytes, std::vector<float>& out) {
  constexpr int bits = static_cast<int>(N * 8);
  constexpr double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (bits - 1));
  const std::size_t count = bytes.size() / N;
  const std::size_t start = out.size();
  out.resize(start + count);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t v = load<N, Little>(bytes.data() + i * N);
    std::int64_t s = static_cast<std::int64_t>(v);
    if constexpr (bits < 64) {
      if ((v >> (bits - 1)) & 1u) s -= static_cast<std::int64_t>(std::uint64_t{1} << bits);
    }
    out[start + i] = static_cast<float>(static_cast<double>(s) * scale);
  }
}

float sanitize(double v) noexcept { return std::isfinite(v) ? static_cast<float>(v) : 0.0f; }

}  // namespace

void decode_pcm(ByteView data, const PcmFormat& format, std::vector<float>& out) {
  const std::span<const std::uint8_t> bytes = as_u8(data);
  const bool little = format.order == std::endian::little;
  switch (format.encoding) {
    case PcmEncoding::Alaw:
      for (const std::uint8_t b : bytes) out.push_back(static_cast<float>(alaw_to_linear(b)) / 32768.0f);
      return;
    case PcmEncoding::Mulaw:
      for (const std::uint8_t b : bytes) out.push_back(static_cast<float>(mulaw_to_linear(b)) / 32768.0f);
      return;
    case PcmEncoding::Float:
      if (format.container_bytes == 4) {
        for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
          const auto v = static_cast<std::uint32_t>(little ? load<4, true>(bytes.data() + i) : load<4, false>(bytes.data() + i));
          out.push_back(sanitize(std::bit_cast<float>(v)));
        }
      } else {
        for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8) {
          const std::uint64_t v = little ? load<8, true>(bytes.data() + i) : load<8, false>(bytes.data() + i);
          out.push_back(sanitize(std::bit_cast<double>(v)));
        }
      }
      return;
    case PcmEncoding::Unsigned: {
      const std::size_t n = format.container_bytes;
      const double half = static_cast<double>(std::uint64_t{1} << (n * 8 - 1));
      for (std::size_t i = 0; i + n <= bytes.size(); i += n) {
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < n; ++k) {
          const std::size_t at = little ? i + n - 1 - k : i + k;
          v = (v << 8) | bytes[at];
        }
        out.push_back(static_cast<float>((static_cast<double>(v) - half) / half));
      }
      return;
    }
    case PcmEncoding::Signed:
      switch (format.container_bytes) {
        case 1: return decode_signed<1, true>(bytes, out);
        case 2: return little ? decode_signed<2, true>(bytes, out) : decode_signed<2, false>(bytes, out);
        case 3: return little ? decode_signed<3, true>(bytes, out) : decode_signed<3, false>(bytes, out);
        case 4: return little ? decode_signed<4, true>(bytes, out) : decode_signed<4, false>(bytes, out);
        default: return;
      }
  }
}

void write_pcm_integers(std::span<const std::int32_t> samples, const PcmFormat& format, util::ByteWriter& out) {
  const int shift = format.container_bytes * 8 - format.bits;
  const std::size_t n = format.container_bytes;
  const bool little = format.order == std::endian::little;
  for (const std::int32_t sample : samples) {
    std::uint64_t v;
    if (format.encoding == PcmEncoding::Unsigned) {
      v = static_cast<std::uint64_t>(static_cast<std::int64_t>(sample) + (std::int64_t{1} << (format.bits - 1)));
    } else {
      v = static_cast<std::uint64_t>(static_cast<std::int64_t>(sample));
    }
    v <<= shift;
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t byte = little ? k : n - 1 - k;
      out.u8(static_cast<std::uint8_t>(v >> (8 * byte)));
    }
  }
}

Bytes encode_pcm(std::span<const float> samples, std::uint16_t channels, const PcmFormat& format, dsp::Dither dither,
                 std::uint32_t sample_rate) {
  util::ByteWriter out(samples.size() * format.container_bytes);
  switch (format.encoding) {
    case PcmEncoding::Float:
      for (const float s : samples) {
        const float clipped = std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
        if (format.container_bytes == 4) {
          if (format.order == std::endian::little) out.f32le(clipped); else out.f32be(clipped);
        } else {
          if (format.order == std::endian::little) out.f64le(clipped); else out.f64be(clipped);
        }
      }
      return out.take();
    case PcmEncoding::Alaw:
    case PcmEncoding::Mulaw: {
      const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels, 16, dither, sample_rate);
      for (const std::int32_t v : ints) {
        const auto s = static_cast<std::int16_t>(v);
        out.u8(format.encoding == PcmEncoding::Alaw ? linear_to_alaw(s) : linear_to_mulaw(s));
      }
      return out.take();
    }
    case PcmEncoding::Signed:
    case PcmEncoding::Unsigned: {
      const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels, format.bits, dither, sample_rate);
      write_pcm_integers(ints, format, out);
      return out.take();
    }
  }
  return out.take();
}

// =========================================================================== G.711

std::int16_t alaw_to_linear(std::uint8_t code) noexcept {
  code ^= 0x55;
  int t = (code & 0x0F) << 4;
  const int segment = (code & 0x70) >> 4;
  switch (segment) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default: t += 0x108; t <<= segment - 1; break;
  }
  return static_cast<std::int16_t>((code & 0x80) ? t : -t);
}

std::int16_t mulaw_to_linear(std::uint8_t code) noexcept {
  code = static_cast<std::uint8_t>(~code);
  int t = ((code & 0x0F) << 3) + 0x84;
  t <<= (code & 0x70) >> 4;
  return static_cast<std::int16_t>((code & 0x80) ? (0x84 - t) : (t - 0x84));
}

namespace {
int segment_of(int value, const std::array<int, 8>& ends) noexcept {
  for (int i = 0; i < 8; ++i) {
    if (value <= ends[static_cast<std::size_t>(i)]) return i;
  }
  return 8;
}
}  // namespace

std::uint8_t linear_to_alaw(std::int16_t sample) noexcept {
  static constexpr std::array<int, 8> ends = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
  int pcm = sample >> 3;
  int mask;
  if (pcm >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    pcm = -pcm - 1;
  }
  const int segment = segment_of(pcm, ends);
  if (segment >= 8) return static_cast<std::uint8_t>(0x7F ^ mask);
  int value = segment << 4;
  value |= segment < 2 ? (pcm >> 1) & 0x0F : (pcm >> segment) & 0x0F;
  return static_cast<std::uint8_t>(value ^ mask);
}

std::uint8_t linear_to_mulaw(std::int16_t sample) noexcept {
  static constexpr std::array<int, 8> ends = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
  int pcm = sample >> 2;
  int mask;
  if (pcm < 0) {
    pcm = -pcm;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  pcm = std::min(pcm, 8159);
  pcm += 0x21;
  const int segment = segment_of(pcm, ends);
  if (segment >= 8) return static_cast<std::uint8_t>(0x7F ^ mask);
  const int value = (segment << 4) | ((pcm >> (segment + 1)) & 0x0F);
  return static_cast<std::uint8_t>(value ^ mask);
}

// =========================================================================== IMA ADPCM

namespace {

constexpr std::array<int, 16> ima_index_table = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr std::array<int, 89> ima_step_table = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct ImaState {
  int predictor = 0;
  int index = 0;

  int expand(int nibble) noexcept {
    const int step = ima_step_table[static_cast<std::size_t>(index)];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    predictor += (nibble & 8) ? -diff : diff;
    predictor = std::clamp(predictor, -32768, 32767);
    index = std::clamp(index + ima_index_table[static_cast<std::size_t>(nibble)], 0, 88);
    return predictor;
  }
  int encode(int sample) noexcept {
    int diff = sample - predictor;
    int code = 0;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    int step = ima_step_table[static_cast<std::size_t>(index)];
    for (int mask = 4; mask != 0; mask >>= 1) {
      if (diff >= step) {
        code |= mask;
        diff -= step;
      }
      step >>= 1;
    }
    expand(code);
    return code;
  }
};

float to_float16(int v) noexcept { return static_cast<float>(v) / 32768.0f; }

}  // namespace

std::size_t ima_wav_frames_per_block(std::size_t block_align, std::uint16_t channels) noexcept {
  if (channels == 0 || block_align < 4u * channels) return 0;
  return (block_align - 4u * channels) * 8u / (4u * channels) + 1u;
}

Result<void> decode_ima_wav(ByteView data, std::uint16_t channels, std::size_t block_align, std::vector<float>& out) {
  const std::size_t per_block = ima_wav_frames_per_block(block_align, channels);
  if (per_block == 0 || (block_align - 4u * channels) % (4u * channels) != 0) {
    return fail(Status::Decode, "invalid IMA ADPCM block size {} for {} channels", block_align, channels);
  }
  const std::span<const std::uint8_t> bytes = as_u8(data);
  std::vector<ImaState> state(channels);
  std::vector<std::int16_t> block(per_block * channels);
  for (std::size_t at = 0; at + 4u * channels <= bytes.size(); at += block_align) {
    const std::size_t size = std::min(block_align, bytes.size() - at);
    const std::uint8_t* b = bytes.data() + at;
    for (std::size_t c = 0; c < channels; ++c) {
      state[c].predictor = static_cast<std::int16_t>(b[c * 4] | (b[c * 4 + 1] << 8));
      state[c].index = std::min<int>(b[c * 4 + 2], 88);
      block[c] = static_cast<std::int16_t>(state[c].predictor);
    }
    std::size_t frames = 1;
    std::size_t pos = 4u * channels;
    while (pos + 4u * channels <= size && frames < per_block) {
      for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t k = 0; k < 4; ++k) {
          const std::uint8_t byte = b[pos + c * 4 + k];
          block[(frames + k * 2) * channels + c] = static_cast<std::int16_t>(state[c].expand(byte & 0x0F));
          block[(frames + k * 2 + 1) * channels + c] = static_cast<std::int16_t>(state[c].expand(byte >> 4));
        }
      }
      frames += 8;
      pos += 4u * channels;
    }
    frames = std::min(frames, per_block);
    for (std::size_t i = 0; i < frames * channels; ++i) out.push_back(to_float16(block[i]));
  }
  return {};
}

Bytes encode_ima_wav(std::span<const std::int16_t> samples, std::uint16_t channels, std::size_t block_align,
                     std::vector<int>* step_indices) {
  util::ByteWriter out;
  const std::size_t per_block = ima_wav_frames_per_block(block_align, channels);
  if (per_block == 0 || channels == 0) return out.take();
  const std::size_t frames = samples.size() / channels;
  std::vector<ImaState> state(channels);
  if (step_indices != nullptr && step_indices->size() == channels) {
    for (std::size_t c = 0; c < channels; ++c) state[c].index = std::clamp((*step_indices)[c], 0, 88);
  }
  const auto sample = [&](std::size_t frame, std::size_t c) -> int {
    return frame < frames ? samples[frame * channels + c] : 0;
  };
  for (std::size_t first = 0; first < frames; first += per_block) {
    for (std::size_t c = 0; c < channels; ++c) {
      state[c].predictor = sample(first, c);
      out.i16le(static_cast<std::int16_t>(state[c].predictor));
      out.u8(static_cast<std::uint8_t>(state[c].index));
      out.u8(0);
    }
    for (std::size_t f = first + 1; f < first + per_block; f += 8) {
      for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t k = 0; k < 4; ++k) {
          const int lo = state[c].encode(sample(f + k * 2, c));
          const int hi = state[c].encode(sample(f + k * 2 + 1, c));
          out.u8(static_cast<std::uint8_t>(lo | (hi << 4)));
        }
      }
    }
  }
  if (step_indices != nullptr) {
    step_indices->resize(channels);
    for (std::size_t c = 0; c < channels; ++c) (*step_indices)[c] = state[c].index;
  }
  return out.take();
}

Result<void> decode_ima4(ByteView data, std::uint16_t channels, std::vector<float>& out) {
  if (channels == 0) return fail(Status::Decode, "IMA4 audio without channels");
  const std::span<const std::uint8_t> bytes = as_u8(data);
  const std::size_t packet = 34u * channels;
  std::vector<ImaState> state(channels);
  std::vector<float> block(64u * channels);
  for (std::size_t at = 0; at + packet <= bytes.size(); at += packet) {
    for (std::size_t c = 0; c < channels; ++c) {
      const std::uint8_t* p = bytes.data() + at + c * 34;
      const int header = (p[0] << 8) | p[1];
      const int predictor = static_cast<std::int16_t>(header & 0xFF80);
      const int index = std::min(header & 0x7F, 88);
      // Keep the running predictor when the header agrees to 9 bits (as the
      // reference decoder does), else resynchronise.
      if ((state[c].predictor & ~0x7F) != predictor || state[c].index != index) {
        state[c].predictor = predictor;
        state[c].index = index;
      }
      for (std::size_t k = 0; k < 32; ++k) {
        block[(k * 2) * channels + c] = to_float16(state[c].expand(p[2 + k] & 0x0F));
        block[(k * 2 + 1) * channels + c] = to_float16(state[c].expand(p[2 + k] >> 4));
      }
    }
    out.insert(out.end(), block.begin(), block.end());
  }
  return {};
}

Bytes encode_ima4(std::span<const std::int16_t> samples, std::uint16_t channels) {
  util::ByteWriter out;
  if (channels == 0) return out.take();
  const std::size_t frames = samples.size() / channels;
  std::vector<ImaState> state(channels);
  for (std::size_t first = 0; first < frames; first += 64) {
    for (std::size_t c = 0; c < channels; ++c) {
      // The header carries the predictor to 9 bits; decoders that track the
      // running predictor (as ours does) continue from the exact value.
      out.u16be(static_cast<std::uint16_t>((state[c].predictor & 0xFF80) | state[c].index));
      for (std::size_t k = 0; k < 32; ++k) {
        const std::size_t f = first + k * 2;
        const int lo = state[c].encode(f < frames ? samples[f * channels + c] : 0);
        const int hi = state[c].encode(f + 1 < frames ? samples[(f + 1) * channels + c] : 0);
        out.u8(static_cast<std::uint8_t>(lo | (hi << 4)));
      }
    }
  }
  return out.take();
}

// =========================================================================== Microsoft ADPCM

namespace {

constexpr std::array<int, 16> ms_adaptation = {230, 230, 230, 230, 307, 409, 512, 614,
                                               768, 614, 512, 409, 307, 230, 230, 230};

struct MsState {
  int c1 = 256;
  int c2 = 0;
  int delta = 16;
  int s1 = 0;
  int s2 = 0;

  int expand(int nibble) noexcept {
    int predictor = (s1 * c1 + s2 * c2) >> 8;  // arithmetic shift, as the reference decoder
    const int signed_nibble = nibble >= 8 ? nibble - 16 : nibble;
    predictor = std::clamp(predictor + signed_nibble * delta, -32768, 32767);
    s2 = s1;
    s1 = predictor;
    delta = std::max(16, (ms_adaptation[static_cast<std::size_t>(nibble)] * delta) >> 8);
    return predictor;
  }
  int encode(int sample) noexcept {
    const int predictor = (s1 * c1 + s2 * c2) >> 8;  // arithmetic shift, as the reference decoder
    const int error = sample - predictor;
    int nibble = error >= 0 ? (error + delta / 2) / delta : -((-error + delta / 2) / delta);
    nibble = std::clamp(nibble, -8, 7);
    expand(nibble & 0x0F);
    return nibble & 0x0F;
  }
};

}  // namespace

MsAdpcmCoefficients ms_adpcm_standard_coefficients() {
  return {{256, 512, 0, 192, 240, 460, 392}, {0, -256, 0, 64, 0, -208, -232}};
}

std::size_t ms_adpcm_frames_per_block(std::size_t block_align, std::uint16_t channels) noexcept {
  if (channels == 0 || block_align < 7u * channels) return 0;
  return (block_align - 7u * channels) * 2u / channels + 2u;
}

Result<void> decode_ms_adpcm(ByteView data, std::uint16_t channels, std::size_t block_align,
                             const MsAdpcmCoefficients& coefficients, std::vector<float>& out) {
  const std::size_t per_block = ms_adpcm_frames_per_block(block_align, channels);
  if (per_block == 0 || channels > 2) {
    return fail(Status::Decode, "invalid MS ADPCM block size {} for {} channels", block_align, channels);
  }
  if (coefficients.c1.empty() || coefficients.c1.size() != coefficients.c2.size()) {
    return fail(Status::Decode, "MS ADPCM without predictor coefficients");
  }
  const std::span<const std::uint8_t> bytes = as_u8(data);
  std::vector<MsState> state(channels);
  for (std::size_t at = 0; at + 7u * channels <= bytes.size(); at += block_align) {
    const std::size_t size = std::min(block_align, bytes.size() - at);
    util::ByteReader r(data.subspan(at, size));
    for (std::size_t c = 0; c < channels; ++c) {
      const std::size_t predictor = r.u8();
      if (predictor >= coefficients.c1.size()) return fail(Status::Decode, "MS ADPCM predictor {} out of range", predictor);
      state[c].c1 = coefficients.c1[predictor];
      state[c].c2 = coefficients.c2[predictor];
    }
    for (std::size_t c = 0; c < channels; ++c) state[c].delta = r.i16le();
    for (std::size_t c = 0; c < channels; ++c) state[c].s1 = r.i16le();
    for (std::size_t c = 0; c < channels; ++c) state[c].s2 = r.i16le();
    for (std::size_t c = 0; c < channels; ++c) out.push_back(to_float16(state[c].s2));
    for (std::size_t c = 0; c < channels; ++c) out.push_back(to_float16(state[c].s1));
    std::size_t produced = 2u * channels;
    const std::size_t wanted = per_block * channels;
    while (!r.at_end() && produced < wanted) {
      const std::uint8_t byte = r.u8();
      for (int half = 0; half < 2 && produced < wanted; ++half) {
        const int nibble = half == 0 ? byte >> 4 : byte & 0x0F;
        const std::size_t c = produced % channels;
        out.push_back(to_float16(state[c].expand(nibble)));
        ++produced;
      }
    }
  }
  return {};
}

Bytes encode_ms_adpcm(std::span<const std::int16_t> samples, std::uint16_t channels, std::size_t block_align) {
  util::ByteWriter out;
  const std::size_t per_block = ms_adpcm_frames_per_block(block_align, channels);
  if (per_block == 0 || channels == 0 || channels > 2) return out.take();
  const MsAdpcmCoefficients coefficients = ms_adpcm_standard_coefficients();
  const std::size_t frames = samples.size() / channels;
  const auto sample = [&](std::size_t frame, std::size_t c) -> int {
    return frame < frames ? samples[frame * channels + c] : 0;
  };
  static constexpr std::array<int, 6> initial_deltas = {16, 48, 128, 384, 1024, 3072};
  for (std::size_t first = 0; first < frames; first += per_block) {
    // Choose predictor and initial delta per channel by trial encoding.
    std::vector<std::size_t> best_predictor(channels, 0);
    std::vector<int> best_delta(channels, 16);
    for (std::size_t c = 0; c < channels; ++c) {
      double best_error = std::numeric_limits<double>::infinity();
      for (std::size_t p = 0; p < coefficients.c1.size(); ++p) {
        for (const int delta : initial_deltas) {
          MsState s;
          s.c1 = coefficients.c1[p];
          s.c2 = coefficients.c2[p];
          s.delta = delta;
          s.s2 = sample(first, c);
          s.s1 = sample(first + 1, c);
          double error = 0.0;
          for (std::size_t f = first + 2; f < first + per_block; ++f) {
            s.encode(sample(f, c));
            const double e = s.s1 - sample(f, c);
            error += e * e;
          }
          if (error < best_error) {
            best_error = error;
            best_predictor[c] = p;
            best_delta[c] = delta;
          }
        }
      }
    }
    std::vector<MsState> state(channels);
    for (std::size_t c = 0; c < channels; ++c) {
      state[c].c1 = coefficients.c1[best_predictor[c]];
      state[c].c2 = coefficients.c2[best_predictor[c]];
      state[c].delta = best_delta[c];
      state[c].s2 = sample(first, c);
      state[c].s1 = sample(first + 1, c);
      out.u8(static_cast<std::uint8_t>(best_predictor[c]));
    }
    for (std::size_t c = 0; c < channels; ++c) out.i16le(static_cast<std::int16_t>(state[c].delta));
    for (std::size_t c = 0; c < channels; ++c) out.i16le(static_cast<std::int16_t>(state[c].s1));
    for (std::size_t c = 0; c < channels; ++c) out.i16le(static_cast<std::int16_t>(state[c].s2));
    int pending = -1;
    for (std::size_t f = first + 2; f < first + per_block; ++f) {
      for (std::size_t c = 0; c < channels; ++c) {
        const int nibble = state[c].encode(sample(f, c));
        if (pending < 0) {
          pending = nibble;
        } else {
          out.u8(static_cast<std::uint8_t>((pending << 4) | nibble));
          pending = -1;
        }
      }
    }
    if (pending >= 0) out.u8(static_cast<std::uint8_t>(pending << 4));
  }
  return out.take();
}

}  // namespace cvt::audio
