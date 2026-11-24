// Convertible core - audio_lossy family: MP3 streams from ir::Audio (with
// the Info/Xing gapless tag) and the Layer III encoder for the codec
// registry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "families/audio_lossy/mpeg/mp3_encoder.hpp"
#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_files.hpp"
#include "families/audio_lossy/mpeg/mpeg_tables.hpp"

namespace cvt::lossy::mpeg {

bool mp3_rate_supported(std::uint32_t rate) noexcept {
  for (const auto& row : sample_rates) {
    for (const std::uint32_t r : row) {
      if (r == rate) return true;
    }
  }
  return false;
}

std::uint32_t nearest_mp3_rate(std::uint32_t rate) noexcept {
  static constexpr std::array<std::uint32_t, 9> rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  for (const std::uint32_t r : rates) {
    if (r >= rate) return r;
  }
  return 48000;
}

Result<Bytes> encode_mp3(const ir::Audio& audio, const Mp3Settings& settings, const Context& ctx) {
  Layer3Encoder encoder;
  CVT_TRY(encoder.configure(audio.sample_rate, audio.channels, settings));
  const std::size_t frames = audio.frame_count();
  const std::size_t chunk = 1152 * 32;
  std::vector<Bytes> out;
  std::uint64_t bytes = 0;
  const auto collect = [&] {
    for (Bytes& f : encoder.take_frames()) {
      bytes += f.size();
      out.push_back(std::move(f));
    }
  };
  for (std::size_t first = 0; first < frames; first += chunk) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(std::max<std::size_t>(frames, 1)), "Encoding"));
    const std::size_t n = std::min(chunk, frames - first);
    encoder.push(audio.samples.data() + first * audio.channels, n);
    CVT_TRY(encoder.encode(false));
    collect();
    CVT_TRY(ctx.limits.check_output(bytes));
  }
  CVT_TRY(encoder.encode(true));
  collect();
  Bytes result;
  const bool vbr = settings.mode != Mp3Settings::Mode::Cbr || !settings.bitrate;
  if (settings.write_info_tag) {
    result = encoder.info_frame(static_cast<std::uint32_t>(out.size()), bytes, vbr);
  }
  result.reserve(result.size() + static_cast<std::size_t>(bytes));
  for (const Bytes& f : out) result.insert(result.end(), f.begin(), f.end());
  CVT_TRY(ctx.limits.check_output(result.size()));
  return result;
}

// =========================================================================== registry encoder

namespace {

class Mp3PacketEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (!mp3_rate_supported(params.sample_rate)) {
      return fail(Status::InvalidArgument, "MP3 does not support {} Hz", params.sample_rate);
    }
    if (params.channels < 1 || params.channels > 2) return fail(Status::InvalidArgument, "MP3 holds one or two channels");
    Mp3Settings s;
    s.quality = params.quality;
    if (params.bitrate_kbps) {
      s.bitrate = *params.bitrate_kbps;
      s.mode = Mp3Settings::Mode::Cbr;
    }
    s.write_info_tag = false;
    channels_ = params.channels;
    rate_ = params.sample_rate;
    timescale_ = params.timescale != 0 ? params.timescale : params.sample_rate;
    return encoder_.configure(params.sample_rate, params.channels, s);
  }
  Result<void> send_chunk(const codec::AudioChunk& chunk) override {
    encoder_.push(chunk.samples.data(), chunk.samples.size() / channels_);
    CVT_TRY(encoder_.encode(false));
    collect();
    return {};
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (ready_.empty()) return std::optional<codec::Packet>{};
    codec::Packet p = std::move(ready_.front());
    ready_.erase(ready_.begin());
    return std::optional<codec::Packet>(std::move(p));
  }
  [[nodiscard]] Bytes extradata() const override { return {}; }
  Result<void> flush() override {
    CVT_TRY(encoder_.encode(true));
    collect();
    return {};
  }
  [[nodiscard]] std::uint32_t frame_size() const override { return encoder_.samples_per_frame(); }
  [[nodiscard]] std::uint32_t delay() const override { return encoder_.encoder_delay() + 529; }

 private:
  void collect() {
    for (Bytes& f : encoder_.take_frames()) {
      codec::Packet p;
      p.bytes = std::move(f);
      const std::uint64_t spf = encoder_.samples_per_frame();
      p.pts = p.dts = static_cast<std::int64_t>(index_ * spf * timescale_ / rate_);
      p.duration = static_cast<std::int64_t>(spf * timescale_ / rate_);
      p.keyframe = true;
      ++index_;
      ready_.push_back(std::move(p));
    }
  }
  Layer3Encoder encoder_;
  std::vector<codec::Packet> ready_;
  std::uint16_t channels_ = 2;
  std::uint32_t rate_ = 44100;
  std::uint32_t timescale_ = 44100;
  std::uint64_t index_ = 0;
};

}  // namespace

std::unique_ptr<codec::AudioEncoder> make_mp3_encoder() { return std::make_unique<Mp3PacketEncoder>(); }

namespace {

/// Layer II for containers: the input is collected and encoded at flush
/// (Layer II frames are independent, so packets are simply its frames).
class Mp2PacketEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.channels < 1 || params.channels > 2) return fail(Status::InvalidArgument, "MP2 holds one or two channels");
    audio_.sample_rate = params.sample_rate;
    audio_.channels = params.channels;
    const int per_channel = params.quality >= 90 ? 128 : params.quality >= 75 ? 96 : 64;
    settings_.bitrate = params.bitrate_kbps.value_or(static_cast<std::uint32_t>(per_channel * params.channels));
    timescale_ = params.timescale != 0 ? params.timescale : params.sample_rate;
    bool ok = false;
    for (const auto& row : sample_rates) {
      for (const std::uint32_t r : row) ok = ok || (r == params.sample_rate && r >= 16000);
    }
    if (!ok) return fail(Status::InvalidArgument, "MP2 does not support {} Hz", params.sample_rate);
    return {};
  }
  Result<void> send_chunk(const codec::AudioChunk& chunk) override {
    audio_.samples.insert(audio_.samples.end(), chunk.samples.begin(), chunk.samples.end());
    return {};
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (next_ >= packets_.size()) return std::optional<codec::Packet>{};
    return std::optional<codec::Packet>(std::move(packets_[next_++]));
  }
  [[nodiscard]] Bytes extradata() const override { return {}; }
  Result<void> flush() override {
    Context ctx;
    CVT_TRY_ASSIGN(Bytes stream, encode_mp2(audio_, settings_, ctx));
    const ByteView data(stream);
    std::size_t pos = 0;
    std::uint64_t index = 0;
    while (data.size() - pos >= 4) {
      const auto h = parse_header(header_word(data, pos));
      if (!h) break;
      const std::size_t size = std::min(h->frame_bytes(h->bitrate_kbps), data.size() - pos);
      codec::Packet p;
      p.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + size));
      p.pts = p.dts = static_cast<std::int64_t>(index * 1152 * timescale_ / audio_.sample_rate);
      p.duration = static_cast<std::int64_t>(1152 * timescale_ / audio_.sample_rate);
      p.keyframe = true;
      packets_.push_back(std::move(p));
      pos += size;
      ++index;
    }
    return {};
  }
  [[nodiscard]] std::uint32_t frame_size() const override { return 1152; }
  [[nodiscard]] std::uint32_t delay() const override { return 481; }

 private:
  ir::Audio audio_;
  Mp2Settings settings_;
  std::vector<codec::Packet> packets_;
  std::size_t next_ = 0;
  std::uint32_t timescale_ = 48000;
};

}  // namespace

std::unique_ptr<codec::AudioEncoder> make_mp2_encoder() { return std::make_unique<Mp2PacketEncoder>(); }

}  // namespace cvt::lossy::mpeg
