// Convertible core - audio_lossy family: AAC decoding through the imported
// PacketVideo decoder (AAC-LC, HE-AAC v1/v2, mono and stereo) and AAC-LC
// encoding through the imported VisualOn encoder, behind in-house classes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/audio_lossless/bits.hpp"
#include "families/audio_lossy/aac/aac.hpp"

#include "families/audio_lossy/aac/pv_aacdec_rename.h"
#include "families/audio_lossy/aac/vo_aacenc_rename.h"
#include <pvmp4audiodecoder_api.h>
#include <cmnMemory.h>
#include <voAAC.h>

namespace cvt::lossy::aac {

namespace {

constexpr std::array<std::uint32_t, 13> rates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                 22050, 16000, 12000, 11025, 8000,  7350};

}  // namespace

int rate_index(std::uint32_t rate) noexcept {
  for (std::size_t i = 0; i < rates.size(); ++i) {
    if (rates[i] == rate) return static_cast<int>(i);
  }
  return 15;
}

std::uint32_t rate_from_index(int index) noexcept {
  return index >= 0 && index < static_cast<int>(rates.size()) ? rates[static_cast<std::size_t>(index)] : 0;
}

std::optional<AudioConfig> parse_audio_config(ByteView asc) {
  audio::BitReader r(asc);
  AudioConfig c;
  const auto object_type = [&] {
    int t = static_cast<int>(r.read(5));
    if (t == 31) t = 32 + static_cast<int>(r.read(6));
    return t;
  };
  const auto rate = [&]() -> std::uint32_t {
    const int index = static_cast<int>(r.read(4));
    return index == 15 ? r.read(24) : rate_from_index(index);
  };
  c.object_type = object_type();
  c.sample_rate = rate();
  c.channel_config = static_cast<int>(r.read(4));
  if (c.object_type == 5 || c.object_type == 29) {
    c.sbr = true;
    c.ps = c.object_type == 29;
    c.extension_rate = rate();
    c.object_type = object_type();
  }
  if (r.overrun() || c.sample_rate == 0) return std::nullopt;
  // Backward-compatible signalling of SBR/PS after the GASpecificConfig.
  if (!c.sbr && (c.object_type == 2 || c.object_type == 4)) {
    r.read(3);  // frameLengthFlag, dependsOnCoreCoder (assumed 0), extensionFlag
    if (r.remaining_bits() >= 16 && r.peek(11) == 0x2B7) {
      r.read(11);
      if (object_type() == 5) {
        if (r.read(1) != 0) {
          c.sbr = true;
          c.extension_rate = rate();
          if (r.remaining_bits() >= 12 && r.read(11) == 0x548 && r.read(1) != 0) c.ps = true;
        }
      }
    }
  }
  return c;
}

Bytes make_lc_config(std::uint32_t sample_rate, int channels) {
  audio::BitWriter w;
  w.write(2, 5);
  const int index = rate_index(sample_rate);
  w.write(static_cast<std::uint32_t>(index), 4);
  if (index == 15) w.write(sample_rate, 24);
  w.write(static_cast<std::uint32_t>(channels), 4);
  w.write(0, 3);  // GASpecificConfig: 1024-sample frames, no core coder, no extension
  const auto bytes = w.take();
  return to_bytes(std::span<const std::uint8_t>(bytes));
}

// =========================================================================== decoder

struct Decoder::Impl {
  std::vector<std::uint8_t> memory;
  tPVMP4AudioDecoderExternal ext{};
  std::array<std::int16_t, 16384> pcm{};
  std::vector<std::uint8_t> input;
  int channels = 0;
  std::uint32_t rate = 0;
  bool configured = false;
  bool sbr = false;
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

namespace {

/// An AudioSpecificConfig given inside an MPEG-4 ES_Descriptor (CAF 'kuki',
/// QuickTime 'esds' bodies): the payload of its DecoderSpecificInfo.
ByteView unwrap_descriptor(ByteView data) {
  const auto d = as_u8(data);
  std::size_t pos = 0;
  if (d.size() >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0) pos = 4;  // full-box header
  if (pos >= d.size() || d[pos] != 0x03) return data;
  const auto length = [&](std::size_t& at) {
    std::size_t n = 0;
    for (int i = 0; i < 4 && at < d.size(); ++i) {
      const std::uint8_t b = d[at++];
      n = (n << 7) | (b & 0x7Fu);
      if ((b & 0x80) == 0) break;
    }
    return n;
  };
  ++pos;
  length(pos);
  if (pos + 3 > d.size()) return data;
  const std::uint8_t flags = d[pos + 2];
  pos += 3;
  if ((flags & 0x80) != 0) pos += 2;
  if ((flags & 0x40) != 0 && pos < d.size()) pos += 1 + d[pos];
  if ((flags & 0x20) != 0) pos += 2;
  if (pos >= d.size() || d[pos] != 0x04) return data;
  ++pos;
  length(pos);
  pos += 13;
  if (pos >= d.size() || d[pos] != 0x05) return data;
  ++pos;
  const std::size_t n = length(pos);
  if (pos > d.size() || n > d.size() - pos) return data;
  return data.subspan(pos, n);
}

}  // namespace

Result<void> Decoder::configure(ByteView given) {
  Impl& m = *impl_;
  const ByteView asc = unwrap_descriptor(given);
  const auto config = parse_audio_config(asc);
  if (!config) return fail(Status::Decode, "invalid AAC AudioSpecificConfig");
  if (config->object_type != 2 && config->object_type != 1 && config->object_type != 4) {
    return fail(Status::Decode, "unsupported AAC object type {}", config->object_type);
  }
  if (config->channel_config < 1 || config->channel_config > 2) {
    return fail(Status::Decode, "AAC with {} channels is not supported (mono and stereo only)",
                config->channel_config == 0 ? std::string("program-defined") : std::to_string(config->channel_config));
  }
  m.memory.assign(PVMP4AudioDecoderGetMemRequirements(), 0);
  std::memset(&m.ext, 0, sizeof(m.ext));
  m.ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
  m.ext.aacPlusEnabled = true;
  // Parametric stereo is not built (it crashes this decoder on 64-bit
  // targets): HE-AAC v2 plays as its mono HE-AAC core, as PS-unaware
  // decoders do.
  m.ext.desiredChannels = config->channel_config == 2 ? 2 : 1;
  if (PVMP4AudioDecoderInitLibrary(&m.ext, m.memory.data()) != 0) return fail(Status::Decode, "the AAC decoder failed to start");
  // The decoder detects SBR and PS in the bitstream itself (implicit
  // signalling); explicit hierarchical configurations are given to it in
  // their backward-compatible AAC-LC form.
  const Bytes plain = make_lc_config(config->sample_rate, config->channel_config);
  m.input.assign(plain.size() + 16, 0);
  std::memcpy(m.input.data(), plain.data(), plain.size());
  m.ext.pInputBuffer = m.input.data();
  m.ext.inputBufferCurrentLength = static_cast<Int>(plain.size());
  m.ext.inputBufferMaxLength = 0;
  if (PVMP4AudioDecoderConfig(&m.ext, m.memory.data()) != MP4AUDEC_SUCCESS) {
    return fail(Status::Decode, "the AAC decoder rejected the stream configuration");
  }
  m.channels = m.ext.desiredChannels;
  m.rate = config->sbr && config->extension_rate != 0 ? config->extension_rate : config->sample_rate;
  m.configured = true;
  return {};
}

Result<std::uint32_t> Decoder::decode(ByteView unit, std::vector<float>& out) {
  Impl& m = *impl_;
  if (!m.configured) return fail(Status::Decode, "the AAC decoder is not configured");
  if (unit.size() > 8192) return fail(Status::Decode, "AAC access unit is too large");
  // The decoder may read a few bytes ahead: give it a padded copy.
  m.input.assign(unit.size() + 4096, 0);  // damaged data elements may claim up to 510 more bytes
  std::memcpy(m.input.data(), unit.data(), unit.size());
  m.ext.pInputBuffer = m.input.data();
  m.ext.inputBufferCurrentLength = static_cast<Int>(unit.size());
  m.ext.inputBufferMaxLength = 0;
  m.ext.inputBufferUsedLength = 0;
  m.ext.remainderBits = 0;
  m.ext.pOutputBuffer = m.pcm.data();
  m.ext.pOutputBuffer_plus = m.pcm.data() + 2048;
  m.ext.repositionFlag = false;
  const Int err = PVMP4AudioDecodeFrame(&m.ext, m.memory.data());
  const int up = m.ext.aacPlusUpsamplingFactor == 2 ? 2 : 1;
  const std::uint32_t frames = static_cast<std::uint32_t>(m.ext.frameLength > 0 ? m.ext.frameLength : 1024) * static_cast<std::uint32_t>(up);
  if (up == 2) m.sbr = true;
  if (m.ext.samplingRate > 0) m.rate = static_cast<std::uint32_t>(m.ext.samplingRate);
  const std::size_t ch = static_cast<std::size_t>(m.channels);
  const std::size_t at = out.size();
  if (err != MP4AUDEC_SUCCESS) {
    // A damaged access unit becomes silence of the right length.
    out.resize(at + frames * ch, 0.0f);
    return fail(Status::Decode, "damaged AAC access unit");
  }
  out.resize(at + frames * ch);
  if (up == 2 && ch == 1) {
    // Mono with SBR: the second half of the frame is in the "plus" buffer.
    for (std::size_t i = 0; i < 1024; ++i) out[at + i] = m.pcm[i] / 32768.0f;
    for (std::size_t i = 0; i < 1024; ++i) out[at + 1024 + i] = m.pcm[2048 + i] / 32768.0f;
  } else {
    for (std::size_t i = 0; i < frames * ch; ++i) out[at + i] = m.pcm[i] / 32768.0f;
  }
  return frames;
}

std::uint32_t Decoder::sample_rate() const noexcept { return impl_->rate; }
int Decoder::channels() const noexcept { return impl_->channels; }
// Two QMF banks of 481 samples each at the output rate (measured against
// the priming that Apple's and Fraunhofer's encoders record).
std::uint32_t Decoder::extra_delay() const noexcept { return impl_->sbr ? 962 : 0; }

// =========================================================================== encoder

struct Encoder::Impl {
  VO_AUDIO_CODECAPI api{};
  VO_MEM_OPERATOR mem{};
  VO_CODEC_INIT_USERDATA user{};
  VO_HANDLE handle = nullptr;
  std::vector<std::int16_t> input;
  std::vector<std::uint8_t> output;
  int channels = 0;
  std::uint32_t rate = 0;
  ~Impl() {
    if (handle != nullptr) api.Uninit(handle);
  }
};

Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}
Encoder::~Encoder() = default;

Result<void> Encoder::configure(std::uint32_t sample_rate, int channels, std::uint32_t bitrate) {
  Impl& m = *impl_;
  if (channels < 1 || channels > 2) return fail(Status::Encode, "the AAC encoder handles mono and stereo");
  if (rate_index(sample_rate) == 15 || sample_rate < 8000 || sample_rate > 48000) {
    return fail(Status::Encode, "the AAC encoder does not support {} Hz", sample_rate);
  }
  voGetAACEncAPI(&m.api);
  m.mem.Alloc = cmnMemAlloc;
  m.mem.Copy = cmnMemCopy;
  m.mem.Free = cmnMemFree;
  m.mem.Set = cmnMemSet;
  m.mem.Check = cmnMemCheck;
  m.user.memflag = VO_IMF_USERMEMOPERATOR;
  m.user.memData = &m.mem;
  if (m.api.Init(&m.handle, VO_AUDIO_CodingAAC, &m.user) != VO_ERR_NONE || m.handle == nullptr) {
    return fail(Status::Encode, "the AAC encoder failed to start");
  }
  // The encoder's ceiling is 6144 bits per channel and frame.
  const std::uint32_t ceiling = static_cast<std::uint32_t>(6144ull * static_cast<unsigned>(channels) * sample_rate / 1024ull);
  AACENC_PARAM params{};
  params.sampleRate = static_cast<int>(sample_rate);
  params.bitRate = static_cast<int>(std::clamp<std::uint32_t>(bitrate, 8000u * static_cast<unsigned>(channels), ceiling - 1000));
  params.nChannels = static_cast<short>(channels);
  params.adtsUsed = 0;
  if (m.api.SetParam(m.handle, VO_PID_AAC_ENCPARAM, &params) != VO_ERR_NONE) {
    return fail(Status::Encode, "the AAC encoder rejected {} Hz, {} channels at {} bit/s", sample_rate, channels, bitrate);
  }
  m.channels = channels;
  m.rate = sample_rate;
  m.input.resize(1024 * static_cast<std::size_t>(channels));
  m.output.resize(8192);
  return {};
}

Result<Bytes> Encoder::encode(const float* samples) {
  Impl& m = *impl_;
  for (std::size_t i = 0; i < m.input.size(); ++i) {
    const float v = std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f;
    m.input[i] = static_cast<std::int16_t>(v < 0 ? v - 0.5f : v + 0.5f);
  }
  VO_CODECBUFFER in{}, out{};
  VO_AUDIO_OUTPUTINFO info{};
  in.Buffer = reinterpret_cast<VO_PBYTE>(m.input.data());
  in.Length = static_cast<VO_U32>(m.input.size() * sizeof(std::int16_t));
  m.api.SetInputData(m.handle, &in);
  out.Buffer = m.output.data();
  out.Length = static_cast<VO_U32>(m.output.size());
  if (m.api.GetOutputData(m.handle, &out, &info) != VO_ERR_NONE) return fail(Status::Encode, "the AAC encoder failed");
  return to_bytes(std::span<const std::uint8_t>(m.output.data(), out.Length));
}

Bytes Encoder::config() const { return make_lc_config(impl_->rate, impl_->channels); }

}  // namespace cvt::lossy::aac
