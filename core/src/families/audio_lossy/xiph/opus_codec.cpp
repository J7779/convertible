// Convertible core - audio_lossy family: Opus (RFC 6716) through the
// imported libopus, in Ogg (RFC 7845: OpusHead/OpusTags, pre-skip, granule
// positions, end trimming, channel mapping families 0 and 1) and as a codec
// for other containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/xiph/ogg_container.hpp"
#include "families/audio_lossy/xiph/xiph.hpp"
#include "util/bytes.hpp"

// The library's names carry a private prefix (see opus_rename.h).
#include "families/audio_lossy/xiph/opus_rename.h"
#include <opus.h>
#include <opus_multistream.h>

namespace cvt::lossy::xiph {

namespace {

constexpr int max_frame = 5760;  // 120 ms at 48 kHz

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
  return p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

struct MsDecoderDeleter {
  void operator()(OpusMSDecoder* d) const noexcept { opus_multistream_decoder_destroy(d); }
};
struct MsEncoderDeleter {
  void operator()(OpusMSEncoder* e) const noexcept { opus_multistream_encoder_destroy(e); }
};

/// libopus multistream decoder producing WAVE-ordered float samples.
class Decoder {
 public:
  Result<void> open(const OpusHead& head) {
    if (head.channels < 1 || head.channels > 255) return fail(Status::Decode, "invalid Opus channel count");
    int err = 0;
    dec_.reset(opus_multistream_decoder_create(48000, head.channels, head.streams, head.coupled, head.mapping.data(), &err));
    if (!dec_ || err != OPUS_OK) return fail(Status::Decode, "unsupported Opus stream configuration");
    if (head.gain_q8 != 0) opus_multistream_decoder_ctl(dec_.get(), OPUS_SET_GAIN(head.gain_q8));
    channels_ = head.channels;
    buffer_.resize(static_cast<std::size_t>(max_frame) * static_cast<std::size_t>(channels_));
    return {};
  }
  /// Appends the decoded samples; a damaged packet becomes concealment.
  Result<int> decode(ByteView packet, std::vector<float>& out) {
    const auto* data = reinterpret_cast<const unsigned char*>(packet.data());
    const auto size = static_cast<opus_int32>(std::min<std::size_t>(packet.size(), 1 << 20));
    int n = opus_multistream_decode_float(dec_.get(), size == 0 ? nullptr : data, size, buffer_.data(), max_frame, 0);
    if (n < 0) {
      // Packet loss concealment for a damaged packet keeps the timeline.
      int duration = packet.empty() ? 960 : opus_packet_get_nb_samples(data, size, 48000);
      if (duration <= 0 || duration > max_frame) duration = 960;
      n = opus_multistream_decode_float(dec_.get(), nullptr, 0, buffer_.data(), duration, 0);
      if (n < 0) return fail(Status::Decode, "the Opus stream is damaged");
      damaged_ = true;
    }
    const std::size_t count = static_cast<std::size_t>(n) * static_cast<std::size_t>(channels_);
    const std::size_t at = out.size();
    out.insert(out.end(), buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
    vorbis_to_wave(std::span<float>(out).subspan(at), channels_);
    return n;
  }
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  std::unique_ptr<OpusMSDecoder, MsDecoderDeleter> dec_;
  std::vector<float> buffer_;
  int channels_ = 0;
  bool damaged_ = false;
};

/// libopus multistream encoder fed with WAVE-ordered samples at 48 kHz.
class Encoder {
 public:
  Result<void> open(int channels, const OpusSettings& s) {
    int err = 0;
    int streams = 0, coupled = 0;
    head_.channels = channels;
    head_.family = channels <= 2 ? 0 : 1;
    enc_.reset(opus_multistream_surround_encoder_create(48000, channels, head_.family, &streams, &coupled,
                                                         head_.mapping.data(), OPUS_APPLICATION_AUDIO, &err));
    if (!enc_ || err != OPUS_OK) return fail(Status::Encode, "Opus cannot encode {} channels", channels);
    head_.streams = streams;
    head_.coupled = coupled;
    opus_multistream_encoder_ctl(enc_.get(), OPUS_SET_BITRATE(static_cast<opus_int32>(s.bitrate_bps)));
    opus_multistream_encoder_ctl(enc_.get(), OPUS_SET_COMPLEXITY(s.complexity));
    opus_multistream_encoder_ctl(enc_.get(), OPUS_SET_VBR(s.vbr ? 1 : 0));
    opus_int32 lookahead = 0;
    opus_multistream_encoder_ctl(enc_.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    head_.pre_skip = lookahead;
    head_.input_rate = 48000;
    channels_ = channels;
    packet_.resize(4000 * static_cast<std::size_t>(std::max(1, streams)));
    return {};
  }
  /// Encode one 20 ms frame (960 WAVE-ordered sample frames).
  Result<Bytes> encode(const float* wave_samples) {
    frame_.assign(wave_samples, wave_samples + 960 * static_cast<std::size_t>(channels_));
    wave_to_vorbis(frame_, channels_);
    const int n = opus_multistream_encode_float(enc_.get(), frame_.data(), 960, packet_.data(),
                                                static_cast<opus_int32>(packet_.size()));
    if (n < 0) return fail(Status::Encode, "the Opus encoder failed");
    return to_bytes(std::span<const std::uint8_t>(packet_.data(), static_cast<std::size_t>(n)));
  }
  [[nodiscard]] const OpusHead& head() const noexcept { return head_; }

 private:
  std::unique_ptr<OpusMSEncoder, MsEncoderDeleter> enc_;
  OpusHead head_;
  std::vector<float> frame_;
  std::vector<std::uint8_t> packet_;
  int channels_ = 0;
};

}  // namespace

std::optional<OpusHead> parse_opus_head(ByteView data) {
  const auto d = as_u8(data);
  OpusHead h;
  std::size_t map_at = 0;
  if (d.size() >= 19 && std::memcmp(d.data(), "OpusHead", 8) == 0) {
    if ((d[8] & 0xF0) != 0) return std::nullopt;  // incompatible major version
    h.channels = d[9];
    h.pre_skip = le16(d.data() + 10);
    h.input_rate = le32(d.data() + 12);
    h.gain_q8 = static_cast<std::int16_t>(le16(d.data() + 16));
    h.family = d[18];
    map_at = 19;
  } else if (d.size() >= 11 && d[0] == 0) {
    // ISO BMFF 'dOps': the same fields, big-endian, without the magic.
    h.channels = d[1];
    h.pre_skip = be16(d.data() + 2);
    h.input_rate = be32(d.data() + 4);
    h.gain_q8 = static_cast<std::int16_t>(be16(d.data() + 8));
    h.family = d[10];
    map_at = 11;
  } else {
    return std::nullopt;
  }
  if (h.channels == 0) return std::nullopt;
  if (h.family == 0) {
    if (h.channels > 2) return std::nullopt;
    h.streams = 1;
    h.coupled = h.channels == 2 ? 1 : 0;
    h.mapping[0] = 0;
    h.mapping[1] = 1;
    return h;
  }
  if (d.size() < map_at + 2 + static_cast<std::size_t>(h.channels)) return std::nullopt;
  h.streams = d[map_at];
  h.coupled = d[map_at + 1];
  if (h.streams == 0 || h.coupled > h.streams || h.streams + h.coupled > 255) return std::nullopt;
  for (int c = 0; c < h.channels; ++c) {
    h.mapping[static_cast<std::size_t>(c)] = d[map_at + 2 + static_cast<std::size_t>(c)];
    const int m = h.mapping[static_cast<std::size_t>(c)];
    if (m != 255 && m >= h.streams + h.coupled) return std::nullopt;
  }
  return h;
}

Bytes make_opus_head(const OpusHead& head) {
  util::ByteWriter w;
  w.text("OpusHead");
  w.u8(1);
  w.u8(static_cast<std::uint8_t>(head.channels));
  w.u16le(static_cast<std::uint16_t>(head.pre_skip));
  w.u32le(head.input_rate);
  w.u16le(static_cast<std::uint16_t>(static_cast<std::int16_t>(head.gain_q8)));
  w.u8(static_cast<std::uint8_t>(head.family));
  if (head.family != 0) {
    w.u8(static_cast<std::uint8_t>(head.streams));
    w.u8(static_cast<std::uint8_t>(head.coupled));
    for (int c = 0; c < head.channels; ++c) w.u8(head.mapping[static_cast<std::size_t>(c)]);
  }
  return w.take();
}

Result<ir::Audio> read_ogg_opus(ByteView input, const Context& ctx) {
  CVT_TRY_ASSIGN(OggLogicalStream stream, ogg_find_stream(input, "OpusHead", ctx));
  const auto head = parse_opus_head(stream.packets.front());
  if (!head) return fail(Status::Decode, "invalid OpusHead header");
  if (head->channels > 8 && head->family != 255) return fail(Status::Decode, "unsupported Opus channel layout");
  ir::Audio audio;
  audio.sample_rate = 48000;
  audio.channels = static_cast<std::uint16_t>(head->channels);
  std::size_t first_audio = 1;
  if (stream.packets.size() > 1 && stream.packets[1].size() >= 8 && std::memcmp(stream.packets[1].data(), "OpusTags", 8) == 0) {
    audio::tags::parse_vorbis_comment(ByteView(stream.packets[1]).subspan(8), audio.tags);
    first_audio = 2;
  }
  Decoder decoder;
  CVT_TRY(decoder.open(*head));
  std::int64_t last_granule = -1;
  std::uint64_t decoded = 0;
  for (std::size_t i = first_audio; i < stream.packets.size(); ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(stream.packets.size()), "Decoding"));
    }
    CVT_TRY_ASSIGN(const int n, decoder.decode(stream.packets[i], audio.samples));
    decoded += static_cast<std::uint64_t>(n);
    CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
    if (stream.granules[i] >= 0) last_granule = stream.granules[i];
  }
  if (decoder.damaged() || stream.damaged) ctx.warn("Damaged Opus packets were concealed.");
  // Trim the encoder's pre-skip and, from the last granule position, the
  // padding of the final packet.
  std::uint64_t keep = decoded;
  if (last_granule >= 0 && stream.complete) {
    keep = last_granule > head->pre_skip ? static_cast<std::uint64_t>(last_granule - head->pre_skip) : 0;
  }
  const std::uint64_t skip = std::min<std::uint64_t>(static_cast<std::uint64_t>(head->pre_skip), decoded);
  keep = std::min(keep, decoded - skip);
  const std::size_t ch = audio.channels;
  audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * ch));
  audio.samples.resize(static_cast<std::size_t>(keep) * ch);
  return audio;
}

Result<Bytes> write_ogg_opus(const ir::Audio& audio, const OpusSettings& settings, const Context& ctx) {
  if (audio.sample_rate != 48000) return fail(Status::Encode, "Opus streams are written at 48 kHz");
  Encoder encoder;
  CVT_TRY(encoder.open(audio.channels, settings));
  const OpusHead& head = encoder.head();
  const std::uint64_t frames = audio.frame_count();
  const std::uint64_t total = frames + static_cast<std::uint64_t>(head.pre_skip);
  const std::uint64_t packets = (total + 959) / 960;
  OggWriter ogg(0x4F505553u);  // fixed serial: identical input gives identical output
  ogg.add(make_opus_head(head), 0, true);
  util::ByteWriter tags;
  tags.text("OpusTags");
  const Bytes comments = audio::tags::vorbis_comment(audio.tags);
  tags.bytes(comments);
  ogg.add(tags.view(), 0, true);
  const std::size_t ch = audio.channels;
  std::vector<float> frame(960 * ch);
  for (std::uint64_t p = 0; p < packets; ++p) {
    if ((p & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(p) / static_cast<double>(packets), "Encoding"));
    std::fill(frame.begin(), frame.end(), 0.0f);
    const std::uint64_t first = p * 960;
    if (first < frames) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(960, frames - first));
      std::copy_n(audio.samples.begin() + static_cast<std::ptrdiff_t>(first * ch), n * ch, frame.begin());
    }
    CVT_TRY_ASSIGN(Bytes packet, encoder.encode(frame.data()));
    const auto granule = static_cast<std::int64_t>(std::min<std::uint64_t>((p + 1) * 960, total));
    ogg.add(packet, granule, false);
    CVT_TRY(ctx.limits.check_output(ogg.size()));
  }
  return ogg.finish();
}

// =========================================================================== registry

namespace {

class OpusPacketDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    std::optional<OpusHead> head = parse_opus_head(params.extradata);
    if (!head) {
      // No configuration: assume a plain mono/stereo stream.
      OpusHead h;
      h.channels = params.channels == 1 ? 1 : 2;
      h.coupled = h.channels == 2 ? 1 : 0;
      h.mapping[0] = 0;
      h.mapping[1] = 1;
      head = h;
    }
    head_ = *head;
    skip_ = static_cast<std::uint64_t>(head_.pre_skip);
    return decoder_.open(head_);
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    std::vector<float> out;
    CVT_TRY_ASSIGN(const int decoded, decoder_.decode(packet.bytes, out));
    (void)decoded;
    const std::size_t ch = static_cast<std::size_t>(head_.channels);
    const std::uint64_t frames = out.size() / ch;
    const std::uint64_t drop = std::min(skip_, frames);
    skip_ -= drop;
    pending_.insert(pending_.end(), out.begin() + static_cast<std::ptrdiff_t>(drop * ch), out.end());
    return {};
  }
  Result<std::optional<codec::AudioChunk>> receive_chunk() override {
    if (pending_.empty()) return std::optional<codec::AudioChunk>{};
    codec::AudioChunk chunk;
    chunk.samples = std::move(pending_);
    pending_.clear();
    return std::optional<codec::AudioChunk>(std::move(chunk));
  }
  Result<void> flush() override { return {}; }
  [[nodiscard]] std::uint32_t sample_rate() const override { return 48000; }
  [[nodiscard]] std::uint16_t channels() const override { return static_cast<std::uint16_t>(head_.channels); }

 private:
  Decoder decoder_;
  OpusHead head_;
  std::uint64_t skip_ = 0;
  std::vector<float> pending_;
};

class OpusPacketEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.sample_rate != 48000) {
      return fail(Status::InvalidArgument, "the Opus encoder takes 48 kHz input (got {} Hz)", params.sample_rate);
    }
    if (params.channels < 1 || params.channels > 8) return fail(Status::InvalidArgument, "Opus encodes 1 to 8 channels");
    OpusSettings s;
    const double per_channel = params.quality >= 90 ? 80.0 : params.quality >= 75 ? 64.0 : params.quality >= 50 ? 48.0 : 32.0;
    s.bitrate_bps = params.bitrate_kbps ? *params.bitrate_kbps * 1000 : static_cast<std::uint32_t>(per_channel * 1000 * params.channels);
    channels_ = params.channels;
    timescale_ = params.timescale != 0 ? params.timescale : 48000;
    return encoder_.open(params.channels, s);
  }
  Result<void> send_chunk(const codec::AudioChunk& chunk) override {
    buffer_.insert(buffer_.end(), chunk.samples.begin(), chunk.samples.end());
    return drain(false);
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (next_ >= ready_.size()) {
      ready_.clear();
      next_ = 0;
      return std::optional<codec::Packet>{};
    }
    return std::optional<codec::Packet>(std::move(ready_[next_++]));
  }
  [[nodiscard]] Bytes extradata() const override { return make_opus_head(encoder_.head()); }
  Result<void> flush() override { return drain(true); }
  [[nodiscard]] std::uint32_t frame_size() const override { return 960; }
  [[nodiscard]] std::uint32_t delay() const override { return static_cast<std::uint32_t>(encoder_.head().pre_skip); }

 private:
  Result<void> drain(bool final) {
    const std::size_t per = 960 * channels_;
    std::size_t pos = 0;
    if (final && buffer_.size() % per != 0) buffer_.resize((buffer_.size() / per + 1) * per, 0.0f);
    if (final) buffer_.resize(buffer_.size() + per, 0.0f);  // flush the lookahead
    while (buffer_.size() - pos >= per) {
      CVT_TRY_ASSIGN(Bytes bytes, encoder_.encode(buffer_.data() + pos));
      codec::Packet p;
      p.bytes = std::move(bytes);
      p.pts = p.dts = static_cast<std::int64_t>(index_ * 960 * timescale_ / 48000);
      p.duration = static_cast<std::int64_t>(960 * timescale_ / 48000);
      p.keyframe = true;
      ready_.push_back(std::move(p));
      ++index_;
      pos += per;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    return {};
  }
  Encoder encoder_;
  std::vector<float> buffer_;
  std::vector<codec::Packet> ready_;
  std::size_t next_ = 0;
  std::size_t channels_ = 2;
  std::uint64_t index_ = 0;
  std::uint32_t timescale_ = 48000;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_opus_decoder() { return std::make_unique<OpusPacketDecoder>(); }
std::unique_ptr<codec::AudioEncoder> make_opus_encoder() { return std::make_unique<OpusPacketEncoder>(); }

}  // namespace cvt::lossy::xiph
